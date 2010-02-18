/*
    SSSD

    Kerberos 5 Backend Module -- tgt_req and changepw child

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2009 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <popt.h>

#include <security/pam_modules.h>

#include "util/util.h"
#include "util/user_info_msg.h"
#include "providers/child_common.h"
#include "providers/dp_backend.h"
#include "providers/krb5/krb5_auth.h"
#include "providers/krb5/krb5_utils.h"

struct krb5_child_ctx {
    /* opts taken from kinit */
    /* in seconds */
    krb5_deltat starttime;
    krb5_deltat lifetime;
    krb5_deltat rlife;

    int forwardable;
    int proxiable;
    int addresses;

    int not_forwardable;
    int not_proxiable;
    int no_addresses;

    int verbose;

    char* principal_name;
    char* service_name;
    char* keytab_name;
    char* k5_cache_name;
    char* k4_cache_name;

    action_type action;

    char *kdcip;
    char *realm;
    char *changepw_principle;
    char *ccache_dir;
    char *ccname_template;
    int auth_timeout;

    int child_debug_fd;
};

struct krb5_req {
    krb5_context ctx;
    krb5_principal princ;
    char* name;
    krb5_creds *creds;
    krb5_get_init_creds_opt *options;
    pid_t child_pid;
    int read_from_child_fd;
    int write_to_child_fd;

    struct be_req *req;
    struct pam_data *pd;
    struct krb5_child_ctx *krb5_ctx;
    errno_t (*child_req)(int fd, struct krb5_req *kr);

    char *ccname;
    char *keytab;
    bool validate;
};

static krb5_context krb5_error_ctx;
static const char *__krb5_error_msg;
#define KRB5_DEBUG(level, krb5_error) do { \
    __krb5_error_msg = sss_krb5_get_error_message(krb5_error_ctx, krb5_error); \
    DEBUG(level, ("%d: [%d][%s]\n", __LINE__, krb5_error, __krb5_error_msg)); \
    sss_krb5_free_error_message(krb5_error_ctx, __krb5_error_msg); \
} while(0);

static krb5_error_code create_empty_cred(struct krb5_req *kr, krb5_creds **_cred)
{
    krb5_error_code kerr;
    krb5_creds *cred = NULL;
    krb5_data *krb5_realm;

    cred = calloc(sizeof(krb5_creds), 1);
    if (cred == NULL) {
        DEBUG(1, ("calloc failed.\n"));
        return ENOMEM;
    }

    kerr = krb5_copy_principal(kr->ctx, kr->princ, &cred->client);
    if (kerr != 0) {
        DEBUG(1, ("krb5_copy_principal failed.\n"));
        goto done;
    }

    krb5_realm = krb5_princ_realm(kr->ctx, kr->princ);

    kerr = krb5_build_principal_ext(kr->ctx, &cred->server,
                                    krb5_realm->length, krb5_realm->data,
                                    KRB5_TGS_NAME_SIZE, KRB5_TGS_NAME,
                                    krb5_realm->length, krb5_realm->data, 0);
    if (kerr != 0) {
        DEBUG(1, ("krb5_build_principal_ext failed.\n"));
        goto done;
    }

done:
    if (kerr != 0) {
        if (cred != NULL && cred->client != NULL) {
            krb5_free_principal(kr->ctx, cred->client);
        }

        free(cred);
    } else {
        *_cred = cred;
    }

    return kerr;
}

static krb5_error_code create_ccache_file(struct krb5_req *kr, krb5_creds *creds)
{
    krb5_error_code kerr;
    krb5_ccache tmp_cc = NULL;
    char *cc_file_name;
    int fd = -1;
    size_t ccname_len;
    char *dummy;
    char *tmp_ccname;
    krb5_creds *l_cred;

    if (strncmp(kr->ccname, "FILE:", 5) == 0) {
        cc_file_name = kr->ccname + 5;
    } else {
        cc_file_name = kr->ccname;
    }

    if (cc_file_name[0] != '/') {
        DEBUG(1, ("Ccache filename is not an absolute path.\n"));
        return EINVAL;
    }

    dummy = strrchr(cc_file_name, '/');
    tmp_ccname = talloc_strndup(kr, cc_file_name, (size_t) (dummy-cc_file_name));
    if (tmp_ccname == NULL) {
        DEBUG(1, ("talloc_strdup failed.\n"));
        return ENOMEM;
    }
    tmp_ccname = talloc_asprintf_append(tmp_ccname, "/.krb5cc_dummy_XXXXXX");

    fd = mkstemp(tmp_ccname);
    if (fd == -1) {
        DEBUG(1, ("mkstemp failed [%d][%s].\n", errno, strerror(errno)));
        return errno;
    }

    kerr = krb5_cc_resolve(kr->ctx, tmp_ccname, &tmp_cc);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto done;
    }

    kerr = krb5_cc_initialize(kr->ctx, tmp_cc, kr->princ);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto done;
    }
    if (fd != -1) {
        close(fd);
        fd = -1;
    }

    if (creds == NULL) {
        kerr = create_empty_cred(kr, &l_cred);
        if (kerr != 0) {
            KRB5_DEBUG(1, kerr);
            goto done;
        }
    } else {
        l_cred = creds;
    }

    kerr = krb5_cc_store_cred(kr->ctx, tmp_cc, l_cred);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto done;
    }

    kerr = krb5_cc_close(kr->ctx, tmp_cc);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto done;
    }
    tmp_cc = NULL;

    ccname_len = strlen(cc_file_name);
    if (ccname_len >= 6 && strcmp(cc_file_name + (ccname_len-6), "XXXXXX")==0 ) {
        fd = mkstemp(cc_file_name);
        if (fd == -1) {
            DEBUG(1, ("mkstemp failed [%d][%s].\n", errno, strerror(errno)));
            kerr = errno;
            goto done;
        }
    }

    kerr = rename(tmp_ccname, cc_file_name);
    if (kerr == -1) {
        DEBUG(1, ("rename failed [%d][%s].\n", errno, strerror(errno)));
    }

done:
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
    if (kerr != 0 && tmp_cc != NULL) {
        krb5_cc_destroy(kr->ctx, tmp_cc);
    }
    return kerr;
}

static struct response *init_response(TALLOC_CTX *mem_ctx) {
    struct response *r;
    r = talloc(mem_ctx, struct response);
    r->buf = talloc_size(mem_ctx, MAX_CHILD_MSG_SIZE);
    if (r->buf == NULL) {
        DEBUG(1, ("talloc_size failed.\n"));
        return NULL;
    }
    r->max_size = MAX_CHILD_MSG_SIZE;
    r->size = 0;

    return r;
}

static errno_t pack_response_packet(struct response *resp, int status, int type,
                                    size_t len, const uint8_t *data)
{
    int p=0;

    if ((3*sizeof(int32_t) + len +1) > resp->max_size) {
        DEBUG(1, ("response message too big.\n"));
        return ENOMEM;
    }

    COPY_INT32_VALUE(&resp->buf[p], status, p);
    COPY_INT32_VALUE(&resp->buf[p], type, p);
    COPY_INT32_VALUE(&resp->buf[p], len, p);
    COPY_MEM(&resp->buf[p], data, p, len);

    resp->size = p;

    return EOK;
}

static struct response *prepare_response_message(struct krb5_req *kr,
                                                 krb5_error_code kerr,
                                                 char *user_error_message,
                                                 int pam_status)
{
    char *msg = NULL;
    const char *krb5_msg = NULL;
    int ret;
    struct response *resp;
    size_t user_resp_len;
    uint8_t *user_resp;

    resp = init_response(kr);
    if (resp == NULL) {
        DEBUG(1, ("init_response failed.\n"));
        return NULL;
    }

    if (kerr == 0) {
        if(kr->pd->cmd == SSS_PAM_CHAUTHTOK_PRELIM) {
            ret = pack_response_packet(resp, PAM_SUCCESS, SSS_PAM_SYSTEM_INFO,
                                       strlen("success") + 1,
                                       (const uint8_t *) "success");
        } else {
            if (kr->ccname == NULL) {
                DEBUG(1, ("Error obtaining ccname.\n"));
                return NULL;
            }

            msg = talloc_asprintf(kr, "%s=%s",CCACHE_ENV_NAME, kr->ccname);
            if (msg == NULL) {
                DEBUG(1, ("talloc_asprintf failed.\n"));
                return NULL;
            }

            ret = pack_response_packet(resp, PAM_SUCCESS, SSS_PAM_ENV_ITEM,
                                       strlen(msg) + 1, (uint8_t *) msg);
            talloc_zfree(msg);
        }
    } else {

        if (user_error_message != NULL) {
            ret = pack_user_info_chpass_error(kr, user_error_message,
                                              &user_resp_len, &user_resp);
            if (ret != EOK) {
                DEBUG(1, ("pack_user_info_chpass_error failed.\n"));
                talloc_zfree(user_error_message);
            } else {
                ret = pack_response_packet(resp, pam_status, SSS_PAM_USER_INFO,
                                           user_resp_len, user_resp);
                if (ret != EOK) {
                    DEBUG(1, ("pack_response_packet failed.\n"));
                    talloc_zfree(user_error_message);
                }
            }
        }

        if (user_error_message == NULL) {
            krb5_msg = sss_krb5_get_error_message(krb5_error_ctx, kerr);
            if (krb5_msg == NULL) {
                DEBUG(1, ("sss_krb5_get_error_message failed.\n"));
                return NULL;
            }

            ret = pack_response_packet(resp, pam_status, SSS_PAM_SYSTEM_INFO,
                                       strlen(krb5_msg) + 1,
                                       (const uint8_t *) krb5_msg);
            sss_krb5_free_error_message(krb5_error_ctx, krb5_msg);
        } else {

        }

    }

    if (ret != EOK) {
        DEBUG(1, ("pack_response_packet failed.\n"));
        return NULL;
    }

    return resp;
}

static errno_t sendresponse(int fd, krb5_error_code kerr,
                            char *user_error_message, int pam_status,
                            struct krb5_req *kr)
{
    struct response *resp;
    size_t written;
    int ret;

    resp = prepare_response_message(kr, kerr, user_error_message, pam_status);
    if (resp == NULL) {
        DEBUG(1, ("prepare_response_message failed.\n"));
        return ENOMEM;
    }

    written = 0;
    while (written < resp->size) {
        ret = write(fd, resp->buf + written, resp->size - written);
        if (ret == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            ret = errno;
            DEBUG(1, ("write failed [%d][%s].\n", ret, strerror(ret)));
            return ret;
        }
        written += ret;
    }

    return EOK;
}

static krb5_error_code validate_tgt(struct krb5_req *kr)
{
    krb5_error_code kerr;
    krb5_error_code kt_err;
    char *principal;
    krb5_keytab keytab;
    krb5_kt_cursor cursor;
    krb5_keytab_entry entry;
    krb5_verify_init_creds_opt opt;

    memset(&keytab, 0, sizeof(keytab));
    kerr = krb5_kt_resolve(kr->ctx, kr->keytab, &keytab);
    if (kerr != 0) {
        DEBUG(1, ("error resolving keytab [%s], not verifying TGT.\n",
                  kr->keytab));
        return kerr;
    }

    memset(&cursor, 0, sizeof(cursor));
    kerr = krb5_kt_start_seq_get(kr->ctx, keytab, &cursor);
    if (kerr != 0) {
        DEBUG(1, ("error reading keytab [%s], not verifying TGT.\n",
                  kr->keytab));
        return kerr;
    }

    /* We look for the first entry from our realm or take the last one */
    memset(&entry, 0, sizeof(entry));
    while ((kt_err = krb5_kt_next_entry(kr->ctx, keytab, &entry, &cursor)) == 0) {
        if (krb5_realm_compare(kr->ctx, entry.principal, kr->princ)) {
            DEBUG(9, ("Found keytab entry with the realm of the credential.\n"));
            break;
        }

        kerr = krb5_free_keytab_entry_contents(kr->ctx, &entry);
        if (kerr != 0) {
            DEBUG(1, ("Failed to free keytab entry.\n"));
        }
        memset(&entry, 0, sizeof(entry));
    }

    /* Close the keytab here.  Even though we're using cursors, the file
     * handle is stored in the krb5_keytab structure, and it gets
     * overwritten when the verify_init_creds() call below creates its own
     * cursor, creating a leak. */
    kerr = krb5_kt_end_seq_get(kr->ctx, keytab, &cursor);
    if (kerr != 0) {
        DEBUG(1, ("krb5_kt_end_seq_get failed, not verifying TGT.\n"));
        goto done;
    }

    /* check if we got any errors from krb5_kt_next_entry */
    if (kt_err != 0 && kt_err != KRB5_KT_END) {
        DEBUG(1, ("error reading keytab [%s], not verifying TGT.\n",
                  kr->keytab));
        goto done;
    }

    /* Get the principal to which the key belongs, for logging purposes. */
    principal = NULL;
    kerr = krb5_unparse_name(kr->ctx, entry.principal, &principal);
    if (kerr != 0) {
        DEBUG(1, ("internal error parsing principal name, "
                  "not verifying TGT.\n"));
        goto done;
    }


    krb5_verify_init_creds_opt_init(&opt);
    kerr = krb5_verify_init_creds(kr->ctx, kr->creds, entry.principal, keytab,
                                  NULL, &opt);

    if (kerr == 0) {
        DEBUG(5, ("TGT verified using key for [%s].\n", principal));
    } else {
        DEBUG(1 ,("TGT failed verification using key for [%s].\n", principal));
    }

done:
    if (krb5_kt_close(kr->ctx, keytab) != 0) {
        DEBUG(1, ("krb5_kt_close failed"));
    }
    if (krb5_free_keytab_entry_contents(kr->ctx, &entry) != 0) {
        DEBUG(1, ("Failed to free keytab entry.\n"));
    }
    if (principal != NULL) {
        sss_krb5_free_unparsed_name(kr->ctx, principal);
    }

    return kerr;

}

static krb5_error_code get_and_save_tgt(struct krb5_req *kr,
                                        char *password)
{
    krb5_error_code kerr = 0;
    int ret;

    kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                        password, NULL, NULL, 0, NULL,
                                        kr->options);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        return kerr;
    }

    if (kr->validate) {
        kerr = validate_tgt(kr);
        if (kerr != 0) {
            KRB5_DEBUG(1, kerr);
            return kerr;
        }

        /* We drop root privileges which were needed to read the keytab file
         * for the validation validation of the credentials here to run the
         * ccache I/O operations with user privileges. */
        ret = become_user(kr->pd->pw_uid, kr->pd->gr_gid);
        if (ret != EOK) {
            DEBUG(1, ("become_user failed.\n"));
            return ret;
        }
    } else {
        DEBUG(9, ("TGT validation is disabled.\n"));
    }

    kerr = create_ccache_file(kr, kr->creds);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto done;
    }

    kerr = 0;

done:
    krb5_free_cred_contents(kr->ctx, kr->creds);

    return kerr;

}

static errno_t changepw_child(int fd, struct krb5_req *kr)
{
    int ret;
    krb5_error_code kerr = 0;
    char *pass_str = NULL;
    char *newpass_str = NULL;
    int pam_status = PAM_SYSTEM_ERR;
    int result_code = -1;
    krb5_data result_code_string;
    krb5_data result_string;
    char *user_error_message = NULL;

    pass_str = talloc_strndup(kr, (const char *) kr->pd->authtok,
                              kr->pd->authtok_size);
    if (pass_str == NULL) {
        DEBUG(1, ("talloc_strndup failed.\n"));
        kerr = KRB5KRB_ERR_GENERIC;
        goto sendresponse;
    }

    kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                        pass_str, NULL, NULL, 0,
                                        kr->krb5_ctx->changepw_principle,
                                        kr->options);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        if (kerr == KRB5_KDC_UNREACH) {
            pam_status = PAM_AUTHINFO_UNAVAIL;
        }
        goto sendresponse;
    }

    memset(pass_str, 0, kr->pd->authtok_size);
    talloc_zfree(pass_str);
    memset(kr->pd->authtok, 0, kr->pd->authtok_size);

    if (kr->pd->cmd == SSS_PAM_CHAUTHTOK_PRELIM) {
        DEBUG(9, ("Initial authentication for change password operation "
                  "successfull.\n"));
        krb5_free_cred_contents(kr->ctx, kr->creds);
        pam_status = PAM_SUCCESS;
        goto sendresponse;
    }

    newpass_str = talloc_strndup(kr, (const char *) kr->pd->newauthtok,
                              kr->pd->newauthtok_size);
    if (newpass_str == NULL) {
        DEBUG(1, ("talloc_strndup failed.\n"));
        kerr = KRB5KRB_ERR_GENERIC;
        goto sendresponse;
    }

    kerr = krb5_change_password(kr->ctx, kr->creds, newpass_str, &result_code,
                                &result_code_string, &result_string);

    if (kerr != 0 || result_code != 0) {
        if (kerr != 0) {
            KRB5_DEBUG(1, kerr);
        } else {
            kerr = KRB5KRB_ERR_GENERIC;
        }

        if (result_code_string.length > 0) {
            DEBUG(1, ("krb5_change_password failed [%d][%.*s].\n", result_code,
                      result_code_string.length, result_code_string.data));
            user_error_message = talloc_strndup(kr->pd, result_code_string.data,
                                                result_code_string.length);
            if (user_error_message == NULL) {
                DEBUG(1, ("talloc_strndup failed.\n"));
            }
        }

        if (result_string.length > 0) {
            DEBUG(1, ("krb5_change_password failed [%d][%.*s].\n", result_code,
                      result_string.length, result_string.data));
            talloc_free(user_error_message);
            user_error_message = talloc_strndup(kr->pd, result_string.data,
                                                result_string.length);
            if (user_error_message == NULL) {
                DEBUG(1, ("talloc_strndup failed.\n"));
            }
        }

        pam_status = PAM_AUTHTOK_ERR;
        goto sendresponse;
    }

    krb5_free_cred_contents(kr->ctx, kr->creds);

    kerr = get_and_save_tgt(kr, newpass_str);
    memset(newpass_str, 0, kr->pd->newauthtok_size);
    talloc_zfree(newpass_str);
    memset(kr->pd->newauthtok, 0, kr->pd->newauthtok_size);

    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        if (kerr == KRB5_KDC_UNREACH) {
            pam_status = PAM_AUTHINFO_UNAVAIL;
        }
    }

sendresponse:
    ret = sendresponse(fd, kerr, user_error_message, pam_status, kr);
    if (ret != EOK) {
        DEBUG(1, ("sendresponse failed.\n"));
    }

    return ret;
}

static errno_t tgt_req_child(int fd, struct krb5_req *kr)
{
    int ret;
    krb5_error_code kerr = 0;
    char *pass_str = NULL;
    int pam_status = PAM_SYSTEM_ERR;

    pass_str = talloc_strndup(kr, (const char *) kr->pd->authtok,
                              kr->pd->authtok_size);
    if (pass_str == NULL) {
        DEBUG(1, ("talloc_strndup failed.\n"));
        kerr = KRB5KRB_ERR_GENERIC;
        goto sendresponse;
    }

    kerr = get_and_save_tgt(kr, pass_str);

    /* If the password is expired the KDC will always return
       KRB5KDC_ERR_KEY_EXP regardless if the supplied password is correct or
       not. In general the password can still be used to get a changepw ticket.
       So we validate the password by trying to get a changepw ticket. */
    if (kerr == KRB5KDC_ERR_KEY_EXP) {
        kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                            pass_str, NULL, NULL, 0,
                                            kr->krb5_ctx->changepw_principle,
                                            kr->options);
        krb5_free_cred_contents(kr->ctx, kr->creds);
        if (kerr == 0) {
            kerr = KRB5KDC_ERR_KEY_EXP;
        }
    }

    memset(pass_str, 0, kr->pd->authtok_size);
    talloc_zfree(pass_str);
    memset(kr->pd->authtok, 0, kr->pd->authtok_size);

    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        switch (kerr) {
            case KRB5_KDC_UNREACH:
                    pam_status = PAM_AUTHINFO_UNAVAIL;
                    break;
            case KRB5KDC_ERR_KEY_EXP:
                    pam_status = PAM_AUTHTOK_EXPIRED;
                    break;
            case KRB5KDC_ERR_PREAUTH_FAILED:
                    pam_status = PAM_CRED_ERR;
                    break;
            default:
                    pam_status = PAM_SYSTEM_ERR;
        }
    }

sendresponse:
    ret = sendresponse(fd, kerr, NULL, pam_status, kr);
    if (ret != EOK) {
        DEBUG(1, ("sendresponse failed.\n"));
    }

    return ret;
}

static errno_t create_empty_ccache(int fd, struct krb5_req *kr)
{
    int ret;
    int pam_status = PAM_SUCCESS;

    ret = create_ccache_file(kr, NULL);
    if (ret != 0) {
        KRB5_DEBUG(1, ret);
        pam_status = PAM_SYSTEM_ERR;
    }

    ret = sendresponse(fd, ret, NULL, pam_status, kr);
    if (ret != EOK) {
        DEBUG(1, ("sendresponse failed.\n"));
    }

    return ret;
}

static errno_t unpack_buffer(uint8_t *buf, size_t size, struct pam_data *pd,
                             char **ccname, char **keytab, uint32_t *validate,
                             uint32_t *offline)
{
    size_t p = 0;
    uint32_t len;

    COPY_UINT32_CHECK(&pd->cmd, buf + p, p, size);
    COPY_UINT32_CHECK(&pd->pw_uid, buf + p, p, size);
    COPY_UINT32_CHECK(&pd->gr_gid, buf + p, p, size);
    COPY_UINT32_CHECK(validate, buf + p, p, size);
    COPY_UINT32_CHECK(offline, buf + p, p, size);

    COPY_UINT32_CHECK(&len, buf + p, p, size);
    if ((p + len ) > size) return EINVAL;
    pd->upn = talloc_strndup(pd, (char *)(buf + p), len);
    if (pd->upn == NULL) return ENOMEM;
    p += len;

    COPY_UINT32_CHECK(&len, buf + p, p, size);
    if ((p + len ) > size) return EINVAL;
    *ccname = talloc_strndup(pd, (char *)(buf + p), len);
    if (*ccname == NULL) return ENOMEM;
    p += len;

    COPY_UINT32_CHECK(&len, buf + p, p, size);
    if ((p + len ) > size) return EINVAL;
    *keytab = talloc_strndup(pd, (char *)(buf + p), len);
    if (*keytab == NULL) return ENOMEM;
    p += len;

    COPY_UINT32_CHECK(&len, buf + p, p, size);
    if ((p + len) > size) return EINVAL;
    pd->authtok = (uint8_t *)talloc_strndup(pd, (char *)(buf + p), len);
    if (pd->authtok == NULL) return ENOMEM;
    pd->authtok_size = len + 1;
    p += len;

    if (pd->cmd == SSS_PAM_CHAUTHTOK) {
        COPY_UINT32_CHECK(&len, buf + p, p, size);

        if ((p + len) > size) return EINVAL;
        pd->newauthtok = (uint8_t *)talloc_strndup(pd, (char *)(buf + p), len);
        if (pd->newauthtok == NULL) return ENOMEM;
        pd->newauthtok_size = len + 1;
        p += len;
    } else {
        pd->newauthtok = NULL;
        pd->newauthtok_size = 0;
    }

    return EOK;
}

static int krb5_cleanup(void *ptr)
{
    struct krb5_req *kr = talloc_get_type(ptr, struct krb5_req);
    if (kr == NULL) return EOK;

    if (kr->options != NULL) {
        sss_krb5_get_init_creds_opt_free(kr->ctx, kr->options);
    }

    if (kr->creds != NULL) {
        krb5_free_cred_contents(kr->ctx, kr->creds);
        krb5_free_creds(kr->ctx, kr->creds);
    }
    if (kr->name != NULL)
        sss_krb5_free_unparsed_name(kr->ctx, kr->name);
    if (kr->princ != NULL)
        krb5_free_principal(kr->ctx, kr->princ);
    if (kr->ctx != NULL)
        krb5_free_context(kr->ctx);

    if (kr->krb5_ctx != NULL) {
        memset(kr->krb5_ctx, 0, sizeof(struct krb5_child_ctx));
    }
    memset(kr, 0, sizeof(struct krb5_req));

    return EOK;
}

static int krb5_setup(struct pam_data *pd, const char *user_princ_str,
                      uint32_t offline, struct krb5_req **krb5_req)
{
    struct krb5_req *kr = NULL;
    krb5_error_code kerr = 0;

    kr = talloc_zero(pd, struct krb5_req);
    if (kr == NULL) {
        DEBUG(1, ("talloc failed.\n"));
        kerr = ENOMEM;
        goto failed;
    }
    talloc_set_destructor((TALLOC_CTX *) kr, krb5_cleanup);

    kr->krb5_ctx = talloc_zero(kr, struct krb5_child_ctx);
    if (kr->krb5_ctx == NULL) {
        DEBUG(1, ("talloc failed.\n"));
        kerr = ENOMEM;
        goto failed;
    }

    kr->krb5_ctx->changepw_principle = getenv(SSSD_KRB5_CHANGEPW_PRINCIPLE);
    if (kr->krb5_ctx->changepw_principle == NULL) {
        DEBUG(1, ("Cannot read [%s] from environment.\n",
                  SSSD_KRB5_CHANGEPW_PRINCIPLE));
        if (pd->cmd == SSS_PAM_CHAUTHTOK) {
            goto failed;
        }
    }

    kr->krb5_ctx->realm = getenv(SSSD_KRB5_REALM);
    if (kr->krb5_ctx->realm == NULL) {
        DEBUG(2, ("Cannot read [%s] from environment.\n", SSSD_KRB5_REALM));
    }

    kr->pd = pd;

    switch(pd->cmd) {
        case SSS_PAM_AUTHENTICATE:
            /* If we are offline, we need to create an empty ccache file */
            if (offline) {
                kr->child_req = create_empty_ccache;
            } else {
                kr->child_req = tgt_req_child;
            }
            break;
        case SSS_PAM_CHAUTHTOK:
        case SSS_PAM_CHAUTHTOK_PRELIM:
            kr->child_req = changepw_child;
            break;
        default:
            DEBUG(1, ("PAM command [%d] not supported.\n", pd->cmd));
            kerr = EINVAL;
            goto failed;
    }

    kerr = krb5_init_context(&kr->ctx);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto failed;
    }

    kerr = krb5_parse_name(kr->ctx, user_princ_str, &kr->princ);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto failed;
    }

    kerr = krb5_unparse_name(kr->ctx, kr->princ, &kr->name);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto failed;
    }

    kr->creds = calloc(1, sizeof(krb5_creds));
    if (kr->creds == NULL) {
        DEBUG(1, ("talloc_zero failed.\n"));
        kerr = ENOMEM;
        goto failed;
    }

    kerr = sss_krb5_get_init_creds_opt_alloc(kr->ctx, &kr->options);
    if (kerr != 0) {
        KRB5_DEBUG(1, kerr);
        goto failed;
    }

/* TODO: set options, e.g.
 *  krb5_get_init_creds_opt_set_tkt_life
 *  krb5_get_init_creds_opt_set_renew_life
 *  krb5_get_init_creds_opt_set_forwardable
 *  krb5_get_init_creds_opt_set_proxiable
 *  krb5_get_init_creds_opt_set_etype_list
 *  krb5_get_init_creds_opt_set_address_list
 *  krb5_get_init_creds_opt_set_preauth_list
 *  krb5_get_init_creds_opt_set_salt
 *  krb5_get_init_creds_opt_set_change_password_prompt
 *  krb5_get_init_creds_opt_set_pa
 */

    *krb5_req = kr;
    return EOK;

failed:
    talloc_free(kr);

    return kerr;
}

int main(int argc, const char *argv[])
{
    uint8_t *buf = NULL;
    int ret;
    ssize_t len = 0;
    struct pam_data *pd = NULL;
    struct krb5_req *kr = NULL;
    char *ccname;
    char *keytab;
    uint32_t validate;
    uint32_t offline;
    int opt;
    poptContext pc;
    int debug_fd = -1;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        {"debug-level", 'd', POPT_ARG_INT, &debug_level, 0,
         _("Debug level"), NULL},
        {"debug-timestamps", 0, POPT_ARG_INT, &debug_timestamps, 0,
         _("Add debug timestamps"), NULL},
        {"debug-fd", 0, POPT_ARG_INT, &debug_fd, 0,
         _("An open file descriptor for the debug logs"), NULL},
        POPT_TABLEEND
    };


    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
        fprintf(stderr, "\nInvalid option %s: %s\n\n",
                  poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            _exit(-1);
        }
    }

    poptFreeContext(pc);

    DEBUG(7, ("krb5_child started.\n"));

    pd = talloc(NULL, struct pam_data);
    if (pd == NULL) {
        DEBUG(1, ("malloc failed.\n"));
        _exit(-1);
    }

    debug_prg_name = talloc_asprintf(pd, "[sssd[krb5_child[%d]]]", getpid());

    if (debug_fd != -1) {
        ret = set_debug_file_from_fd(debug_fd);
        if (ret != EOK) {
            DEBUG(1, ("set_debug_file_from_fd failed.\n"));
        }
    }

    buf = talloc_size(pd, sizeof(uint8_t)*IN_BUF_SIZE);
    if (buf == NULL) {
        DEBUG(1, ("malloc failed.\n"));
        _exit(-1);
    }

    while ((ret = read(STDIN_FILENO, buf + len, IN_BUF_SIZE - len)) != 0) {
        if (ret == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            DEBUG(1, ("read failed [%d][%s].\n", errno, strerror(errno)));
            goto fail;
        } else if (ret > 0) {
            len += ret;
            if (len > IN_BUF_SIZE) {
                DEBUG(1, ("read too much, this should never happen.\n"));
                goto fail;
            }
            continue;
        } else {
            DEBUG(1, ("unexpected return code of read [%d].\n", ret));
            goto fail;
        }
    }
    close(STDIN_FILENO);

    ret = unpack_buffer(buf, len, pd, &ccname, &keytab, &validate, &offline);
    if (ret != EOK) {
        DEBUG(1, ("unpack_buffer failed.\n"));
        goto fail;
    }

    ret = krb5_setup(pd, pd->upn, offline, &kr);
    if (ret != EOK) {
        DEBUG(1, ("krb5_setup failed.\n"));
        goto fail;
    }
    kr->ccname = ccname;
    kr->keytab = keytab;
    kr->validate = (validate == 0) ? false : true;

    ret = kr->child_req(STDOUT_FILENO, kr);
    if (ret != EOK) {
        DEBUG(1, ("Child request failed.\n"));
        goto fail;
    }

    close(STDOUT_FILENO);
    talloc_free(pd);

    return 0;

fail:
    close(STDOUT_FILENO);
    talloc_free(pd);
    exit(-1);
}