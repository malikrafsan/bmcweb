// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#include "mutual_tls.hpp"

#include "sessions.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

extern "C"
{
#include <openssl/obj_mac.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
}

#include "logging.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/verify_context.hpp>

#include <memory>
#include <string_view>

std::string getCommonNameFromCert(X509* cert) {
    std::string commonName;
    // Extract username contained in CommonName
    commonName.resize(256, '\0');
    int length = X509_NAME_get_text_by_NID(
        X509_get_subject_name(cert), NID_commonName, commonName.data(),
        static_cast<int>(commonName.size()));
    if (length <= 0)
    {
        BMCWEB_LOG_DEBUG("TLS cannot get common name to create session");
        return "";
    }
    commonName.resize(static_cast<size_t>(length));
    return commonName;
}

std::string getUPNFromCert(X509* peerCert) {
    GENERAL_NAMES *gs = (GENERAL_NAMES*) X509_get_ext_d2i(peerCert, NID_subject_alt_name, NULL, NULL);
    if (gs == NULL)
        return "";

    std::string upn;
    int i, nid, type;
    for (i = 0; i < sk_GENERAL_NAME_num(gs); i++) {
        GENERAL_NAME *g = sk_GENERAL_NAME_value(gs, i);
        if (g->type != GEN_OTHERNAME) continue;

        nid = OBJ_obj2nid(g->d.otherName->type_id);
        if (nid != NID_ms_upn) continue;

        type = g->d.otherName->value->type;
        if (type != V_ASN1_UTF8STRING) continue;

        upn = std::string(
            (char*)g->d.otherName->value->value.utf8string->data,
            (unsigned int) g->d.otherName->value->value.utf8string->length);
        break;
    }
    GENERAL_NAMES_free(gs);
    return upn;
}

std::string getUsernameFromCert(X509* cert)
{
    const persistent_data::AuthConfigMethods& authMethodsConfig =
        persistent_data::SessionStore::getInstance().getAuthMethodsConfig();
    switch (authMethodsConfig.mTLSCommonNameParsingMode)
    {
        case persistent_data::MTLSCommonNameParseMode::Invalid:
        case persistent_data::MTLSCommonNameParseMode::Whole:
        {
            // Not yet supported
            return "";
        }
        case persistent_data::MTLSCommonNameParseMode::UserPrincipalName:
        {
            std::string upn = getUPNFromCert(cert);
            return upn;
        }
        case persistent_data::MTLSCommonNameParseMode::CommonName:
        {
            std::string commonName = getCommonNameFromCert(cert);
            return std::string{commonName};
        }
        default:
        {
            return "";
        }
    }
}

std::shared_ptr<persistent_data::UserSession> verifyMtlsUser(
    const boost::asio::ip::address& clientIp,
    boost::asio::ssl::verify_context& ctx)
{
    // do nothing if TLS is disabled
    if (!persistent_data::SessionStore::getInstance()
             .getAuthMethodsConfig()
             .tls)
    {
        BMCWEB_LOG_DEBUG("TLS auth_config is disabled");
        return nullptr;
    }

    X509_STORE_CTX* cts = ctx.native_handle();
    if (cts == nullptr)
    {
        BMCWEB_LOG_DEBUG("Cannot get native TLS handle.");
        return nullptr;
    }

    // Get certificate
    X509* peerCert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
    if (peerCert == nullptr)
    {
        BMCWEB_LOG_DEBUG("Cannot get current TLS certificate.");
        return nullptr;
    }

    // Check if certificate is OK
    int ctxError = X509_STORE_CTX_get_error(cts);
    if (ctxError != X509_V_OK)
    {
        BMCWEB_LOG_INFO("Last TLS error is: {}", ctxError);
        return nullptr;
    }

    // Check that we have reached final certificate in chain
    int32_t depth = X509_STORE_CTX_get_error_depth(cts);
    if (depth != 0)
    {
        BMCWEB_LOG_DEBUG(
            "Certificate verification in progress (depth {}), waiting to reach final depth",
            depth);
        return nullptr;
    }

    BMCWEB_LOG_DEBUG("Certificate verification of final depth");

    if (X509_check_purpose(peerCert, X509_PURPOSE_SSL_CLIENT, 0) != 1)
    {
        BMCWEB_LOG_DEBUG(
            "Chain does not allow certificate to be used for SSL client authentication");
        return nullptr;
    }

    std::string sslUser = getUsernameFromCert(peerCert);
    if (sslUser.empty())
    {
        BMCWEB_LOG_WARNING("Failed to get user from peer certificate");
        return nullptr;
    }

    std::string unsupportedClientId;
    return persistent_data::SessionStore::getInstance().generateUserSession(
        sslUser, clientIp, unsupportedClientId,
        persistent_data::SessionType::MutualTLS);
}
