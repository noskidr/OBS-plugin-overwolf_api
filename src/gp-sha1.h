/*
Minimal SHA-1 (FIPS 180-1) — needed only for the RFC 6455 handshake accept key.
Straightforward clean-room implementation of the public algorithm.
Not for cryptographic use beyond the WebSocket handshake.
*/

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace gamepulse {

class Sha1 {
public:
	Sha1() { reset(); }

	void reset()
	{
		h_[0] = 0x67452301u;
		h_[1] = 0xEFCDAB89u;
		h_[2] = 0x98BADCFEu;
		h_[3] = 0x10325476u;
		h_[4] = 0xC3D2E1F0u;
		len_ = 0;
		buf_used_ = 0;
	}

	void update(const void *data, size_t n)
	{
		const uint8_t *p = static_cast<const uint8_t *>(data);
		len_ += static_cast<uint64_t>(n);
		while (n > 0) {
			size_t take = 64 - buf_used_;
			if (take > n)
				take = n;
			std::memcpy(buf_ + buf_used_, p, take);
			buf_used_ += take;
			p += take;
			n -= take;
			if (buf_used_ == 64) {
				process_block(buf_);
				buf_used_ = 0;
			}
		}
	}

	/* digest is 20 bytes */
	void final(uint8_t digest[20])
	{
		uint64_t bit_len = len_ * 8;
		uint8_t pad = 0x80;
		update(&pad, 1);
		uint8_t zero = 0x00;
		while (buf_used_ != 56)
			update(&zero, 1);
		uint8_t len_be[8];
		for (int i = 0; i < 8; i++)
			len_be[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
		/* update() would re-count these 8 bytes into len_, but len_ is
		   no longer read after this point, so feed them directly. */
		update(len_be, 8);
		for (int i = 0; i < 5; i++) {
			digest[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
			digest[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
			digest[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
			digest[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
		}
	}

	static void hash(const std::string &in, uint8_t digest[20])
	{
		Sha1 s;
		s.update(in.data(), in.size());
		s.final(digest);
	}

private:
	static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

	void process_block(const uint8_t block[64])
	{
		uint32_t w[80];
		for (int i = 0; i < 16; i++) {
			w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
			       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
			       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
		}
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

		uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5A827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCu;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6u;
			}
			uint32_t tmp = rol(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = tmp;
		}
		h_[0] += a;
		h_[1] += b;
		h_[2] += c;
		h_[3] += d;
		h_[4] += e;
	}

	uint32_t h_[5];
	uint64_t len_;
	uint8_t buf_[64];
	size_t buf_used_;
};

} // namespace gamepulse
