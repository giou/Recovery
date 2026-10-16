/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "fox_tls_cert.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../data.hpp"
#include "../twcommon.h"

namespace {

const char* kTlsDir = "/data/media/0/Fox/tls";

// Best-effort WLAN IPv4 (used as the cert CN/SAN). Mirrors the lookup Fox_Httpd
// does, kept local to avoid pulling fox_httpd into this translation unit.
std::string local_ip()
{
	std::string ip = DataManager::GetStrValue("wlan_info_ip");
	if (!ip.empty())
		return ip;
	return "127.0.0.1";
}

bool ensure_dir()
{
	mkdir("/data/media/0/Fox", 0700);
	if (mkdir(kTlsDir, 0700) != 0 && errno != EEXIST)
		return false;
	chmod(kTlsDir, 0700);
	return true;
}

// Atomic write with restrictive perms, mirroring Fox_Secrets::Save.
bool write_file_secured(const std::string& path, const std::string& data)
{
	std::string tmp = path + ".tmp";
	{
		std::ofstream ofs(tmp, std::ios::out | std::ios::trunc);
		if (!ofs.is_open())
			return false;
		ofs.write(data.data(), (std::streamsize)data.size());
		ofs.close();
		if (ofs.fail()) {
			unlink(tmp.c_str());
			return false;
		}
	}
	chmod(tmp.c_str(), 0600);
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	chmod(path.c_str(), 0600);
	return true;
}

bool files_usable()
{
	FILE* cf = fopen(Fox_Tls_Cert::CertPath(), "r");
	FILE* kf = fopen(Fox_Tls_Cert::KeyPath(), "r");
	bool ok = cf && kf;
	X509* cert = nullptr;
	EVP_PKEY* key = nullptr;
	if (cf) {
		cert = PEM_read_X509(cf, nullptr, nullptr, nullptr);
		fclose(cf);
	}
	if (kf) {
		key = PEM_read_PrivateKey(kf, nullptr, nullptr, nullptr);
		fclose(kf);
	}
	ok = ok && cert && key;
	X509_free(cert);
	EVP_PKEY_free(key);
	return ok;
}

// bignum -> colon-separated hex string of the DER, used for fingerprint
// formatting below.
std::string to_hex_colon(const unsigned char* d, size_t n)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(n * 3);
	for (size_t i = 0; i < n; i++) {
		if (!out.empty())
			out += ':';
		out += hex[d[i] >> 4];
		out += hex[d[i] & 0xF];
	}
	return out;
}

// Generate a fresh ECDSA P-256 keypair + self-signed cert, persist as PEM.
bool generate(std::string& err)
{
	if (!ensure_dir()) {
		err = "cannot create tls dir";
		return false;
	}

	EVP_PKEY* pkey = nullptr;
	EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
	if (!pctx) {
		err = "EVP_PKEY_CTX_new_id failed";
		return false;
	}
	bool ok = EVP_PKEY_keygen_init(pctx) > 0 &&
	          EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) > 0 &&
	          EVP_PKEY_keygen(pctx, &pkey) > 0;
	EVP_PKEY_CTX_free(pctx);
	if (!ok || !pkey) {
		err = "EC keygen failed";
		EVP_PKEY_free(pkey);
		return false;
	}

	X509* x = X509_new();
	if (!x) {
		err = "X509_new failed";
		EVP_PKEY_free(pkey);
		return false;
	}
	X509_set_version(x, 2);  // v3

	// Bind the generated keypair to the certificate. Without this the
	// TBSCertificate has no SubjectPublicKeyInfo and BoringSSL's X509_sign()
	// fails ("X509_sign failed").
	if (X509_set_pubkey(x, pkey) != 1) {
		err = "X509_set_pubkey failed";
		X509_free(x);
		EVP_PKEY_free(pkey);
		return false;
	}

	unsigned char serial_bytes[8];
	if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) {
		// Fall back to a time-based serial if the urandom path is unavailable.
		for (size_t i = 0; i < sizeof(serial_bytes); i++)
			serial_bytes[i] = (unsigned char)(time(nullptr) >> (i * 4));
	}
	ASN1_INTEGER* serial = X509_get_serialNumber(x);
	// BIGNUM route: keep it simple with ASN1_INTEGER set from bytes.
	BIGNUM* bn = BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr);
	if (bn) {
		BN_to_ASN1_INTEGER(bn, serial);
		BN_free(bn);
	}

	X509_gmtime_adj(X509_get_notBefore(x), 0);
	X509_gmtime_adj(X509_get_notAfter(x), 10L * 365 * 24 * 3600);  // 10 years

	std::string cn = local_ip();
	X509_NAME* name = X509_get_subject_name(x);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	                           (const unsigned char*)cn.c_str(), -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
	                           (const unsigned char*)"OrangeFox", -1, -1, 0);
	X509_set_issuer_name(x, name);

	// subjectAltName: DNS:localhost plus IP:<wlan-ip>. BoringSSL dropped the
	// CONF-string helper (X509V3_EXT_conf_nid), so build the GENERAL_NAMES
	// structure by hand and serialize it with X509V3_EXT_i2d.
	GENERAL_NAMES* gens = GENERAL_NAMES_new();
	if (gens) {
		GENERAL_NAME* dns = GENERAL_NAME_new();
		ASN1_IA5STRING* dns_str = ASN1_IA5STRING_new();
		if (dns && dns_str && ASN1_STRING_set(dns_str, "localhost", -1)) {
			GENERAL_NAME_set0_value(dns, GEN_DNS, dns_str);
			sk_GENERAL_NAME_push(gens, dns);
		} else {
			// OOM: set0_value never ran, so the two halves are still independently
			// owned — free each one that was allocated and skip this SAN entry.
			if (dns) GENERAL_NAME_free(dns);
			if (dns_str) ASN1_STRING_free(dns_str);
		}

		std::string ip = local_ip();
		if (!ip.empty() && ip != "127.0.0.1") {
			ASN1_OCTET_STRING* ip_oct = a2i_IPADDRESS(ip.c_str());
			if (ip_oct) {
				GENERAL_NAME* gip = GENERAL_NAME_new();
				if (gip) {
					GENERAL_NAME_set0_value(gip, GEN_IPADD, ip_oct);
					sk_GENERAL_NAME_push(gens, gip);
				} else {
					ASN1_OCTET_STRING_free(ip_oct);
				}
			}
		}

		X509_EXTENSION* ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, gens);
		if (ext) {
			X509_add_ext(x, ext, -1);
			X509_EXTENSION_free(ext);
		}
		GENERAL_NAMES_free(gens);
	}

	if (X509_sign(x, pkey, EVP_sha256()) == 0) {
		err = "X509_sign failed";
		X509_free(x);
		EVP_PKEY_free(pkey);
		return false;
	}

	// Serialize to PEM in memory, then write atomically.
	std::string cert_pem, key_pem;
	{
		BIO* b = BIO_new(BIO_s_mem());
		PEM_write_bio_X509(b, x);
		BUF_MEM* bm = nullptr;
		BIO_get_mem_ptr(b, &bm);
		cert_pem.assign(bm->data, bm->length);
		BIO_free(b);
	}
	{
		BIO* b = BIO_new(BIO_s_mem());
		PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr);
		BUF_MEM* bm = nullptr;
		BIO_get_mem_ptr(b, &bm);
		key_pem.assign(bm->data, bm->length);
		BIO_free(b);
	}

	X509_free(x);
	EVP_PKEY_free(pkey);

	if (!write_file_secured(Fox_Tls_Cert::CertPath(), cert_pem)) {
		err = "cert write failed";
		return false;
	}
	if (!write_file_secured(Fox_Tls_Cert::KeyPath(), key_pem)) {
		err = "key write failed";
		return false;
	}
	LOGINFO("Fox_Tls_Cert: generated self-signed cert (CN=%s)\n", cn.c_str());
	return true;
}

} // namespace

bool Fox_Tls_Cert::EnsureCert(std::string& cert_path, std::string& key_path, std::string& err)
{
	if (files_usable()) {
		cert_path = CertPath();
		key_path = KeyPath();
		return true;
	}
	if (!generate(err))
		return false;
	cert_path = CertPath();
	key_path = KeyPath();
	return true;
}

bool Fox_Tls_Cert::RegenCert(std::string& err)
{
	// generate() overwrites the files unconditionally.
	return generate(err);
}

std::string Fox_Tls_Cert::Fingerprint()
{
	FILE* f = fopen(CertPath(), "r");
	if (!f)
		return "";
	X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
	fclose(f);
	if (!x)
		return "";

	unsigned char* der = nullptr;
	int n = i2d_X509(x, &der);
	X509_free(x);
	if (n <= 0 || !der)
		return "";

	unsigned char md[32];
	unsigned int md_len = 0;
	EVP_Digest(der, (size_t)n, md, &md_len, EVP_sha256(), nullptr);
	OPENSSL_free(der);
	if (md_len != 32)
		return "";
	return to_hex_colon(md, 32);
}
