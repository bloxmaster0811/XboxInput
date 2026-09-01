//
// GIP auth v1 crypto helpers for the Xbox 360.
//
// The RSA step is the one place that fails silently: the 360's bignum
// format is qword-based and XeCryptBnQwNeRsaPubCrypt expects "Ne" ordering, which does
// NOT match the big-endian byte layout an X.509 modulus comes in.
//
// Rather than guess which conversion is right and debug it through console reboots,
// GipRsaSelfTest() computes a DETERMINISTIC encryption under BOTH plausible conventions
// and logs each result. tools/rsa_check.py computes the same value from the extracted
// modulus, so a single console run settles the convention with certainty.
//
#ifndef GIP_AUTH_H
#define GIP_AUTH_H

#include <xtl.h>
#include <xkelib.h>
#include <stdint.h>

#define RSA2048_BYTES 256
#define RSA2048_QWORDS 32

// ---------------------------------------------------------------------------
// Minimal DER walk to pull the RSA modulus + exponent out of the device's X.509.
// Deliberately not a general ASN.1 parser - it finds the rsaEncryption OID, then the
// BIT STRING that wraps SEQUENCE { INTEGER modulus, INTEGER exponent }.
// Verified against reference/riffmaster_cert.der by tools/parse_cert.py.
// ---------------------------------------------------------------------------
static int GipDerLen(const BYTE* d, int len, int* i) {
	if (*i >= len)
		return -1;
	BYTE b = d[(*i)++];
	if (b < 0x80)
		return b;
	int n = b & 0x7F;
	if (n > 4 || *i + n > len)
		return -1;
	int v = 0;
	while (n--)
		v = (v << 8) | d[(*i)++];
	return v;
}

//
// Returns true and fills modulus (256 bytes, big-endian) + exponent on success.
//
static bool GipCertGetRsaPubKey(const BYTE* der, int len,
                                BYTE* modOut, uint32_t* expOut) {
	// OID 1.2.840.113549.1.1.1 (rsaEncryption)
	static const BYTE oid[] = { 0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
	int p = -1;
	for (int i = 0; i + (int)sizeof(oid) <= len; i++) {
		int k = 0;
		while (k < (int)sizeof(oid) && der[i + k] == oid[k])
			k++;
		if (k == (int)sizeof(oid)) { p = i; break; }
	}
	if (p < 0)
		return false;

	int i = p + sizeof(oid);
	while (i < len && der[i] != 0x03)     // find BIT STRING
		i++;
	if (i >= len)
		return false;
	i++;
	if (GipDerLen(der, len, &i) < 0)
		return false;
	if (i >= len || der[i] != 0x00)       // BIT STRING unused-bits byte
		return false;
	i++;

	if (i >= len || der[i] != 0x30)       // SEQUENCE
		return false;
	i++;
	if (GipDerLen(der, len, &i) < 0)
		return false;

	if (i >= len || der[i] != 0x02)       // INTEGER modulus
		return false;
	i++;
	int mlen = GipDerLen(der, len, &i);
	if (mlen < 0 || i + mlen > len)
		return false;
	// DER may prepend a 0x00 sign byte on a 256-byte modulus.
	if (mlen == RSA2048_BYTES + 1 && der[i] == 0x00) { i++; mlen--; }
	if (mlen != RSA2048_BYTES)
		return false;
	memcpy(modOut, der + i, RSA2048_BYTES);
	i += mlen;

	if (i >= len || der[i] != 0x02)       // INTEGER exponent
		return false;
	i++;
	int elen = GipDerLen(der, len, &i);
	if (elen < 0 || elen > 4 || i + elen > len)
		return false;
	uint32_t e = 0;
	for (int k = 0; k < elen; k++)
		e = (e << 8) | der[i + k];
	*expOut = e;
	return true;
}

// ---------------------------------------------------------------------------
// PKCS#1 v1.5 encryption padding (RFC 2313 block type 02):
//     EM = 0x00 || 0x02 || PS || 0x00 || M
// PS is >= 8 NON-ZERO bytes. For RSA-2048 and a 48-byte message that is 205 bytes.
// ---------------------------------------------------------------------------
static bool GipPkcs1Pad(const BYTE* msg, int msgLen, BYTE* em, bool deterministic) {
	if (msgLen > RSA2048_BYTES - 11)
		return false;

	int psLen = RSA2048_BYTES - 3 - msgLen;
	em[0] = 0x00;
	em[1] = 0x02;

	if (deterministic) {
		for (int i = 0; i < psLen; i++)
			em[2 + i] = 0xFF;              // valid: non-zero, and reproducible
	}
	else {
		XeCryptRandom(em + 2, psLen);
		for (int i = 0; i < psLen; i++)    // PS must contain no zero bytes
			if (em[2 + i] == 0x00)
				em[2 + i] = 0x01;
	}

	em[2 + psLen] = 0x00;
	memcpy(em + 3 + psLen, msg, msgLen);
	return true;
}

// ---------------------------------------------------------------------------
// RSA public-key operation - OUR OWN bignum, not XeCrypt.
//
// XeCryptBnQwNeRsaPubCrypt was tried first and abandoned. Passing raw big-endian
// buffers produced a well-formed but incorrect result, and no permutation of the
// modulus/input/output (tools/rsa_solve.py searched 729 combinations) reproduces it.
// The likeliest explanation is that XECRYPT_RSA.qwReserved must hold a precomputed
// Montgomery constant we have no documentation for. Reverse-engineering an
// undocumented kernel routine through console reboots is not a good trade when the
// operation itself is simple.
//
// Public-key RSA is m^e mod n. With e = 65537 that is 17 modular multiplications.
// Implemented here with 64-bit limbs and a double-and-add modmul: no multiplication
// at all, only shift/add/compare/subtract, which makes it easy to get right.
//
// Cost: ~18 modmuls x 2048 iterations x 32 limbs. Tens of milliseconds. That is
// acceptable for a one-shot handshake step but is NOT something to run inside a USB
// completion callback in production - see the note at the call site.
// ---------------------------------------------------------------------------
#define BN_LIMBS 32                      // 32 x 64 bits = 2048

typedef struct { uint64_t v[BN_LIMBS]; } bn_t;   // v[0] = least significant

static void BnFromBytesBE(bn_t* r, const BYTE* b) {
	for (int i = 0; i < BN_LIMBS; i++) {
		const BYTE* p = b + (BN_LIMBS - 1 - i) * 8;
		uint64_t x = 0;
		for (int k = 0; k < 8; k++)
			x = (x << 8) | p[k];
		r->v[i] = x;
	}
}

static void BnToBytesBE(const bn_t* a, BYTE* b) {
	for (int i = 0; i < BN_LIMBS; i++) {
		BYTE* p = b + (BN_LIMBS - 1 - i) * 8;
		uint64_t x = a->v[i];
		for (int k = 7; k >= 0; k--) {
			p[k] = (BYTE)(x & 0xFF);
			x >>= 8;
		}
	}
}

static int BnCmp(const bn_t* a, const bn_t* b) {
	for (int i = BN_LIMBS - 1; i >= 0; i--) {
		if (a->v[i] != b->v[i])
			return (a->v[i] > b->v[i]) ? 1 : -1;
	}
	return 0;
}

// a -= b (mod 2^2048). Returns borrow out.
static uint64_t BnSub(bn_t* a, const bn_t* b) {
	uint64_t borrow = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		uint64_t ai = a->v[i], bi = b->v[i];
		uint64_t d = ai - bi;
		uint64_t bo = (ai < bi) ? 1 : 0;
		if (d < borrow)
			bo = 1;
		a->v[i] = d - borrow;
		borrow = bo;
	}
	return borrow;
}

// a += b (mod 2^2048). Returns carry out.
static uint64_t BnAdd(bn_t* a, const bn_t* b) {
	uint64_t carry = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		uint64_t s = a->v[i] + b->v[i];
		uint64_t c = (s < a->v[i]) ? 1 : 0;
		uint64_t s2 = s + carry;
		if (s2 < s)
			c = 1;
		a->v[i] = s2;
		carry = c;
	}
	return carry;
}

// a <<= 1. Returns the bit shifted out.
static uint64_t BnShl1(bn_t* a) {
	uint64_t carry = 0;
	for (int i = 0; i < BN_LIMBS; i++) {
		uint64_t nc = a->v[i] >> 63;
		a->v[i] = (a->v[i] << 1) | carry;
		carry = nc;
	}
	return carry;
}

//
// r = (a * b) mod n, with a, b < n and n's top bit set.
//
// Double-and-add: acc starts at 0 and for each bit of b from the top,
//     acc = 2*acc (mod n);  if bit set, acc += a (mod n)
// Because acc < n, both 2*acc and acc+a are < 2n, so a single conditional subtract
// restores the range. When the shift/add overflows 2048 bits the true value is
// 2^2048 + low; subtracting n in plain 2048-bit arithmetic yields exactly the right
// result, because that difference is itself below 2^2048.
//
static void BnModMul(bn_t* r, const bn_t* a, const bn_t* b, const bn_t* n) {
	bn_t acc;
	memset(&acc, 0, sizeof(acc));

	for (int i = BN_LIMBS * 64 - 1; i >= 0; i--) {
		uint64_t c = BnShl1(&acc);
		if (c || BnCmp(&acc, n) >= 0)
			BnSub(&acc, n);

		if ((b->v[i >> 6] >> (i & 63)) & 1) {
			uint64_t c2 = BnAdd(&acc, a);
			if (c2 || BnCmp(&acc, n) >= 0)
				BnSub(&acc, n);
		}
	}
	*r = acc;
}

//
// r = base^e mod n, left-to-right binary exponentiation. e is a small public exponent.
//
static void BnModExp(bn_t* r, const bn_t* base, uint32_t e, const bn_t* n) {
	bn_t result;
	memset(&result, 0, sizeof(result));
	result.v[0] = 1;

	int top = 31;
	while (top > 0 && !((e >> top) & 1))
		top--;

	for (int i = top; i >= 0; i--) {
		bn_t t;
		BnModMul(&t, &result, &result, n);
		result = t;
		if ((e >> i) & 1) {
			BnModMul(&t, &result, base, n);
			result = t;
		}
	}
	*r = result;
}

//
// RSA public-key operation on big-endian 256-byte buffers.
//
static bool GipRsaPubCrypt(const BYTE* modBE, uint32_t pubExp,
                           const BYTE* emBE, BYTE* outBE) {
	static bn_t n, m, c;

	BnFromBytesBE(&n, modBE);
	BnFromBytesBE(&m, emBE);

	if (!(n.v[BN_LIMBS - 1] >> 63))     // expect a full 2048-bit modulus
		return false;
	if (BnCmp(&m, &n) >= 0)             // message must be < modulus
		return false;

	BnModExp(&c, &m, pubExp, &n);
	BnToBytesBE(&c, outBE);
	return true;
}

// ---------------------------------------------------------------------------
// SHA-256.
//
// xkelib does NOT declare these - keXeCrypt.h only lists them in a comment block of
// known ordinals (XeCryptSha256Init @784 ... XeCryptSha256 @787). The symbols ARE
// present in kernelext.lib, so we declare them ourselves. The signature mirrors
// XeCryptSha (keXeCrypt.h:862-875), which takes THREE input buffers and concatenates
// them - convenient, because it removes any need for the undocumented state struct.
// ---------------------------------------------------------------------------
extern "C" {
	NTSYSAPI VOID NTAPI XeCryptSha256(
		IN const PBYTE pbInp1, IN DWORD cbInp1,
		IN const PBYTE pbInp2, IN DWORD cbInp2,
		IN const PBYTE pbInp3, IN DWORD cbInp3,
		OUT PBYTE pbOut, IN DWORD cbOut);
}

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

//
// HMAC-SHA256, built by hand: the kernel exports HMAC only over SHA-1.
//   HMAC(K,m) = H((K' ^ opad) || H((K' ^ ipad) || m))
// The 3-buffer form means each half is a single call, no incremental state needed.
//
static void GipHmacSha256(const BYTE* key, int keyLen,
                          const BYTE* d1, int l1,
                          const BYTE* d2, int l2,
                          BYTE* out) {
	BYTE k[SHA256_BLOCK], ki[SHA256_BLOCK], ko[SHA256_BLOCK], inner[SHA256_DIGEST];

	memset(k, 0, sizeof(k));
	if (keyLen > SHA256_BLOCK)
		XeCryptSha256((PBYTE)key, keyLen, 0, 0, 0, 0, k, SHA256_DIGEST);
	else
		memcpy(k, key, keyLen);

	for (int i = 0; i < SHA256_BLOCK; i++) {
		ki[i] = (BYTE)(k[i] ^ 0x36);
		ko[i] = (BYTE)(k[i] ^ 0x5C);
	}

	// inner = H(ki || d1 || d2)
	XeCryptSha256(ki, SHA256_BLOCK, (PBYTE)d1, l1, (PBYTE)d2, l2, inner, SHA256_DIGEST);
	// out   = H(ko || inner)
	XeCryptSha256(ko, SHA256_BLOCK, inner, SHA256_DIGEST, 0, 0, out, SHA256_DIGEST);
}

// ---------------------------------------------------------------------------
// TLS-1.2-style PRF (P_hash) over HMAC-SHA256.
// refs/xone/auth/crypto.c:79-111:
//
//     A    = HMAC(key, label || seed)
//     loop:  out_block = HMAC(key, A || label || seed)
//            A         = HMAC(key, A)
//
// label||seed is concatenated once so each HMAC needs only two data buffers, which
// is what GipHmacSha256 can take (the third XeCryptSha256 slot holds the padded key).
// ---------------------------------------------------------------------------
#define GIP_PRF_LS_MAX 128

static void GipPrf(const BYTE* key, int keyLen,
                   const char* label,
                   const BYTE* seed, int seedLen,
                   BYTE* out, int outLen) {
	BYTE ls[GIP_PRF_LS_MAX];
	BYTE a[SHA256_DIGEST], blk[SHA256_DIGEST];

	int labLen = 0;
	while (label[labLen])
		labLen++;
	if (labLen + seedLen > GIP_PRF_LS_MAX)
		return;

	memcpy(ls, label, labLen);
	memcpy(ls + labLen, seed, seedLen);
	int lsLen = labLen + seedLen;

	GipHmacSha256(key, keyLen, ls, lsLen, 0, 0, a);          // A(1)

	while (outLen > 0) {
		GipHmacSha256(key, keyLen, a, SHA256_DIGEST, ls, lsLen, blk);
		int n = (outLen < SHA256_DIGEST) ? outLen : SHA256_DIGEST;
		memcpy(out, blk, n);
		out += n;
		outLen -= n;
		GipHmacSha256(key, keyLen, a, SHA256_DIGEST, 0, 0, a);   // A(i+1)
	}
}

// ---------------------------------------------------------------------------
// Handshake transcript.
//
// refs/xone/auth/auth.c:180-181 (sent) and :585-587 (received). The rule is NOT
// symmetric and that asymmetry is easy to miss:
//
//   sent     : hash [6, 6 + data_len)  -> EXCLUDES the 8-byte trailer
//   received : hash [6, len)           -> everything after the handshake header
//
// REQUEST packets and the bare 6-byte acknowledgements are never hashed.
//
// Accumulated into one buffer and hashed in a single call, because the incremental
// XeCryptSha256Init/Update/Final state struct is undocumented in xkelib.
// ---------------------------------------------------------------------------
#define GIP_TRANSCRIPT_MAX 2048

typedef struct {
	BYTE buf[GIP_TRANSCRIPT_MAX];
	int  len;
} GipTranscript;

static void GipTranscriptReset(GipTranscript* t) {
	t->len = 0;
}

static void GipTranscriptAdd(GipTranscript* t, const BYTE* d, int n) {
	if (n <= 0 || t->len + n > GIP_TRANSCRIPT_MAX)
		return;
	memcpy(t->buf + t->len, d, n);
	t->len += n;
}

static void GipTranscriptHash(const GipTranscript* t, BYTE* out32) {
	XeCryptSha256((PBYTE)t->buf, t->len, 0, 0, 0, 0, out32, SHA256_DIGEST);
}

#endif // GIP_AUTH_H
