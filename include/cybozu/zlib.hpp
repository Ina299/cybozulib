#pragma once
/**
	@file
	@brief zlib compressor and decompressor class

	Copyright (C) 2009-2012 Cybozu Inc., all rights reserved.
*/

#include <cybozu/exception.hpp>
#include <cybozu/endian.hpp>
#include <assert.h>
#include <zlib.h>

#ifdef _MSC_VER
	#pragma comment(lib, "zdll.lib")
#endif

namespace cybozu {

struct ZlibException : public cybozu::Exception {
	ZlibException() : cybozu::Exception("zlib") { }
};

namespace zlib_local {

const int DEF_MEM_LEVEL = 8;

} // zlib_local

/**
	zlib compressor class
	OutputStream must have size_t write(const char *buf, size_t size);
*/
template<class OutputStream, size_t maxBufSize = 2048>
class ZlibCompressorT {
	OutputStream& os_;
	unsigned int crc_;
	unsigned int totalSize_; /* mod 2^32 */
	z_stream z_;
	char buf_[maxBufSize];
	bool isFlushCalled_;
	const bool useGzip_;
	ZlibCompressorT(const ZlibCompressorT&);
	void operator=(const ZlibCompressorT&);
public:
	/**
		@param os [in] output stream
		@param useGzip [in] useGzip if true, use deflate if false
		@note useGzip option is not fully tested, so default off
	*/
	ZlibCompressorT(OutputStream& os, bool useGzip = false, int compressionLevel = Z_DEFAULT_COMPRESSION)
		: os_(os)
		, crc_(crc32(0L, Z_NULL, 0))
		, totalSize_(0)
		, isFlushCalled_(false)
		, useGzip_(useGzip)
	{
		z_.zalloc = Z_NULL;
		z_.zfree = Z_NULL;
		z_.opaque = Z_NULL;
		if (useGzip_) {
			if (deflateInit2(&z_, compressionLevel, Z_DEFLATED, -MAX_WBITS, zlib_local::DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
				cybozu::ZlibException e;
				e << "ZlibCompressorT" << "deflateInit2" << std::string(z_.msg);
				throw e;
			}
			char header[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03"; /* OS_CODE = 0x03(Unix) */
			write(header, 10);
		} else {
			if (deflateInit(&z_, compressionLevel) != Z_OK) {
				cybozu::ZlibException e;
				e << "ZlibCompressorT" << "deflateInit" << std::string(z_.msg);
				throw e;
			}
		}
	}
	~ZlibCompressorT()
	{
		assert(isFlushCalled_);
		deflateEnd(&z_);
	}
	/*
		compress buf
		@param buf [in] input data
		@param size [in] input data size
	*/
	void exec(const char *buf, size_t _size)
	{
		assert(_size < (1U << 31));
		uint32_t size = (uint32_t)_size;
		if (useGzip_) {
			crc_ = crc32(crc_, (const Bytef *)buf, size);
			totalSize_ += (unsigned int)size;
		}
		z_.next_in = (Bytef*)buf;
		z_.avail_in = size;
		while (z_.avail_in > 0) {
			z_.next_out = (Bytef*)buf_;
			z_.avail_out = maxBufSize;

			int ret = deflate(&z_, Z_NO_FLUSH);
			if (ret != Z_STREAM_END && ret != Z_OK) {
				cybozu::ZlibException e;
				e << "exec" << "compress" << std::string(z_.msg);
				throw e;
			}
			write(buf_, maxBufSize - z_.avail_out);
			if (ret == Z_STREAM_END) break;
		}
	}
	void flush()
	{
		isFlushCalled_ = true;
		z_.next_in = 0;
		z_.avail_in = 0;

		for (;;) {
			z_.next_out = (Bytef*)buf_;
			z_.avail_out = maxBufSize;

			int ret = deflate(&z_, Z_FINISH);
			if (ret != Z_STREAM_END && ret != Z_OK) {
				cybozu::ZlibException e;
				e << "flush" << std::string(z_.msg);
				throw e;
			}
			write(buf_, sizeof(buf_) - z_.avail_out);
			if (ret == Z_STREAM_END) break;
		}
		if (useGzip_) {
			char tail[8];
			cybozu::Put32bitAsLE(&tail[0], crc_);
			cybozu::Put32bitAsLE(&tail[4], totalSize_);
			write(tail, sizeof(tail));
		}
	}
private:
	void write(const char *buf, size_t _size)
	{
		assert(_size < (1U << 31));
		uint32_t size = (uint32_t)_size;
		size_t writeSize = os_.write(buf, size);
		if (writeSize != size) {
			cybozu::ZlibException e;
			e << "write";
			throw e;
		}
	}
};

/**
	zlib decompressor class
	InputStream must have size_t read(char *str, size_t size);
*/
template<class InputStream, size_t maxBufSize = 2048>
class ZlibDecompressorT {
	InputStream& is_;
	unsigned int crc_;
	unsigned int totalSize_; /* mod 2^32 */
	z_stream z_;
	int ret_;
	char buf_[maxBufSize];
	const bool useGzip_;
	bool readGzipHeader_;
	void readAll(char *buf, size_t size)
	{
		ssize_t readSize = is_.read(buf, size);
		if ((size_t)readSize != size) {
			cybozu::ZlibException e;
			e << "readAll";
			throw e;
		}
	}
	void skipToZero()
	{
		for (;;) {
			char buf[1];
			readAll(buf, 1);
			if (buf[0] == '\0') break;
		}
	}
	void skip(int size)
	{
		for (int i = 0 ; i < size; i++) {
			char buf[1];
			readAll(buf, 1);
		}
	}
	void readGzipHeader()
	{
		char header[10];
		readAll(header, sizeof(header));
		enum {
			FHCRC = 1 << 1,
			FEXTRA = 1 << 2,
			FNAME = 1 << 3,
			FCOMMENT = 1 << 4,
			RESERVED = 7 << 5,
		};
		char flg = header[3];
		if (header[0] == '\x1f'
			&& header[1] == '\x8b'
			&& header[2] == Z_DEFLATED
			&& !(flg & RESERVED)) {
			if (flg & FEXTRA) {
				char xlen[2];
				readAll(xlen, sizeof(xlen));
				int size = cybozu::Get16bitAsLE(xlen);
				skip(size);
			}
			if (flg & FNAME) {
				skipToZero();
			}
			if (flg & FCOMMENT) {
				skipToZero();
			}
			if (flg & FHCRC) {
				skip(2);
			}
			return;
		}
		cybozu::ZlibException e;
		e << "readGzipHeader" << "bad gzip header";
		throw e;
	}
	ZlibDecompressorT(const ZlibDecompressorT&);
	void operator=(const ZlibDecompressorT&);
public:
	/**
		@param os [in] input stream
		@param useGzip [in] useGzip if true, use deflate if false
		@note useGzip option is not fully tested, so default off
	*/
	ZlibDecompressorT(InputStream& is, bool useGzip = false)
		: is_(is)
		, crc_(crc32(0L, Z_NULL, 0))
		, totalSize_(0)
		, ret_(Z_OK)
		, useGzip_(useGzip)
		, readGzipHeader_(false)
	{
		z_.zalloc = Z_NULL;
		z_.zfree = Z_NULL;
		z_.opaque = Z_NULL;
		z_.next_in = 0;
		z_.avail_in = 0;
		if (useGzip_) {
			if (inflateInit2(&z_, -MAX_WBITS) != Z_OK) {
				cybozu::ZlibException e;
				e << "ZlibDecompressorT" << "inflateInit2" << std::string(z_.msg);
				throw e;
			}
		} else {
			if (inflateInit(&z_) != Z_OK) {
				cybozu::ZlibException e;
				e << "ZlibDecompressorT" << "inflateInit" << std::string(z_.msg);
				throw e;
			}
		}
	}
	~ZlibDecompressorT()
	{
		inflateEnd(&z_);
	}
	/*
		decompress is
		@param str [out] decompressed data
		@param str [out] max buf size
		@return written size
	*/
	ssize_t read(char *buf, size_t _size)
	{
		assert(_size < (1U << 31));
		uint32_t size = (uint32_t)_size;
		if (size == 0) return 0;
		if (useGzip_ && !readGzipHeader_) {
			readGzipHeader();
			readGzipHeader_ = true;
		}
		z_.next_out = (Bytef*)buf;
		z_.avail_out = size;
		do {
			if (z_.avail_in == 0) {
				z_.avail_in = (uint32_t)is_.read(buf_, maxBufSize);
				if (ret_ == Z_STREAM_END && z_.avail_in == 0) return 0;
				z_.next_in = (Bytef*)buf_;
			}
			ret_ = inflate(&z_, Z_NO_FLUSH);
			if (ret_ == Z_STREAM_END) break;
			if (ret_ != Z_OK) {
				cybozu::ZlibException e;
				e << "read" << "decompress" << std::string(z_.msg);
				throw e;
			}
		} while (size == z_.avail_out);

		return size - z_.avail_out;
	}
};

} // cybozu

