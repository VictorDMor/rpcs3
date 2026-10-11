#include "Utilities/File.h"
#include "util/serialization_ext.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

extern atomic_t<bool> g_headless;

class zstd_serialization : public testing::Test
{
protected:
	std::filesystem::path directory;
	std::string path;
	bool was_headless = false;

	void SetUp() override
	{
		was_headless = g_headless.exchange(true);
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = std::filesystem::temp_directory_path() / ("rpcs3-serialization-" + std::to_string(stamp));
		ASSERT_TRUE(std::filesystem::create_directory(directory));
		path = (directory / "state.zst").string();
	}

	void TearDown() override
	{
		std::filesystem::remove_all(directory);
		g_headless = was_headless;
	}

	static std::vector<u8> payload(usz size)
	{
		std::vector<u8> result(size);
		u32 random = 0x12345678;
		for (usz i = 0; i < size; i++)
		{
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			result[i] = static_cast<u8>(random);
		}
		return result;
	}

	void write(const std::vector<u8>& data, usz chunk = 0x1000000)
	{
		named_thread capture("Serialization test capture"sv, [&]()
		{
			fs::file file(path, fs::rewrite);
			ASSERT_TRUE(file);
			utils::serial ar;
			ar.m_file_handler = make_compressed_zstd_serialization_file_handler(file);
			for (usz pos = 0; pos < data.size(); pos += chunk)
			{
				ASSERT_TRUE(ar.raw_serialize(data.data() + pos, std::min(chunk, data.size() - pos)));
				ar.breathe(true);
			}
			ar.m_file_handler->finalize(ar);
			ASSERT_TRUE(ar.m_file_handler->is_valid());
		});
		capture();
		ASSERT_EQ(capture, thread_state::finished);
	}

	void reader(utils::serial& ar, bool little = false)
	{
		ar.set_reading_state({}, little);
		ar.m_file_handler = make_compressed_zstd_serialization_file_handler(fs::file(path));
	}

	void roundtrip(const std::vector<u8>& expected)
	{
		utils::serial ar;
		reader(ar);
		std::vector<u8> buffer(0x100003);
		for (usz pos = 0; pos < expected.size();)
		{
			const usz count = std::min(buffer.size(), expected.size() - pos);
			ASSERT_TRUE(ar.raw_serialize(buffer.data(), count));
			ASSERT_EQ(std::memcmp(buffer.data(), expected.data() + pos, count), 0) << pos;
			pos += count;
			ar.breathe();
		}
		ASSERT_TRUE(ar.m_file_handler->is_valid());
	}
};

TEST_F(zstd_serialization, RoundtripAcrossFlushBoundaries)
{
	const auto data = payload(0x2100123);
	write(data);
	roundtrip(data);
}

TEST_F(zstd_serialization, FinalizeReader)
{
	write(payload(4096));
	utils::serial ar;
	reader(ar);
	ASSERT_TRUE(ar.try_read<u64>().first);
	ar.m_file_handler->finalize(ar);
	EXPECT_TRUE(ar.m_file_handler->is_valid());
}

TEST_F(zstd_serialization, InvalidMagicStopsReading)
{
	fs::file file(path, fs::rewrite);
	const auto bytes = payload(64);
	ASSERT_EQ(file.write(bytes.data(), bytes.size()), bytes.size());
	utils::serial ar;
	reader(ar, true);
	EXPECT_FALSE(ar.m_file_handler->handle_file_op(ar, 0, 8, nullptr));
	EXPECT_FALSE(ar.m_file_handler->is_valid());
}

TEST_F(zstd_serialization, LegacyConcatenatedFrames)
{
	const auto data = payload(0x300123);
	fs::file file(path, fs::rewrite);
	std::vector<u8> compressed(ZSTD_compressBound(0x100000));
	for (usz pos = 0; pos < data.size(); pos += 0x100000)
	{
		const usz count = std::min<usz>(0x100000, data.size() - pos);
		const usz size = ZSTD_compress(compressed.data(), compressed.size(), data.data() + pos, count, 8);
		ASSERT_FALSE(ZSTD_isError(size));
		ASSERT_EQ(file.write(compressed.data(), size), size);
	}
	roundtrip(data);
}

TEST_F(zstd_serialization, TruncatedFramesFailWithoutAdvancing)
{
	const auto data = payload(4096);
	write(data);
	fs::file original(path);
	std::vector<u8> compressed(original.size());
	ASSERT_EQ(original.read(compressed.data(), compressed.size()), compressed.size());
	for (usz cut : {usz{0}, usz{4}, usz{10}, compressed.size() - 1})
	{
		SCOPED_TRACE(cut);
		fs::file file(path, fs::rewrite);
		ASSERT_EQ(file.write(compressed.data(), cut), cut);
		utils::serial ar;
		reader(ar, true);
		std::vector<u8> result(data.size(), 0xa5);
		EXPECT_FALSE(ar.raw_serialize(result.data(), result.size()));
		EXPECT_EQ(result, std::vector<u8>(data.size(), 0xa5));
		EXPECT_EQ(ar.pos, 0);
		EXPECT_FALSE(ar.is_valid());
		EXPECT_FALSE(ar.try_read<u64>().first);
	}
}

TEST_F(zstd_serialization, ChecksumCorruptionIsRejected)
{
	write(payload(4096));
	fs::file file(path, fs::read + fs::write);
	const usz last = file.size() - 1;
	u8 byte = 0;
	ASSERT_EQ(file.read_at(last, &byte, 1), 1);
	byte ^= 1;
	file.seek(last);
	ASSERT_EQ(file.write(&byte, 1), 1);
	utils::serial ar;
	reader(ar, true);
	EXPECT_FALSE(ar.try_read<u64>().first);
	EXPECT_FALSE(ar.is_valid());
}

TEST_F(zstd_serialization, EmptyStreamAndCleanEof)
{
	write({});
	utils::serial ar;
	reader(ar, true);
	EXPECT_FALSE(ar.try_read<u64>().first);
	EXPECT_TRUE(ar.is_valid());
	const auto data = payload(4096);
	write(data, 127);
	utils::serial ar2;
	reader(ar2, true);
	std::vector<u8> result(data.size());
	ASSERT_TRUE(ar2.raw_serialize(result.data(), result.size()));
	EXPECT_EQ(result, data);
	EXPECT_FALSE(ar2.try_read<u64>().first);
	EXPECT_TRUE(ar2.is_valid());
}

TEST_F(zstd_serialization, PeekOverflowIsRejected)
{
	utils::serial ar;
	ar.set_reading_state();
	ar.pos = ~usz{0} - 3;
	EXPECT_FALSE(ar.try_read<u64>().first);
	u64 value = 123;
	EXPECT_NE(ar.try_read(value), 0);
	EXPECT_EQ(value, 123);
}

// Run in separate processes for meaningful peak RSS. Optionally replay the first
// 256 MiB of an existing savestate; no guest data is printed or retained.
TEST_F(zstd_serialization, DISABLED_Benchmark)
{
	std::vector<u8> data;
	const char* source = std::getenv("RPCS3_SAVESTATE_BENCH_INPUT");
	const char* kind_env = std::getenv("RPCS3_SAVESTATE_BENCH_KIND");
	const std::string kind = source ? "savestate-prefix" : (kind_env ? kind_env : "mixed");
	if (source)
	{
		fs::file file(source);
		ASSERT_TRUE(file);
		const auto context = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>(ZSTD_createDCtx(), ZSTD_freeDCtx);
		ASSERT_TRUE(context);
		data.resize(0x10000000);
		std::vector<u8> input(0x100000);
		ZSTD_outBuffer out{data.data(), data.size(), 0};
		while (out.pos < out.size)
		{
			const usz size = file.read(input.data(), input.size());
			if (!size) break;
			ZSTD_inBuffer in{input.data(), size, 0};
			while (in.pos < in.size && out.pos < out.size)
			{
				ASSERT_FALSE(ZSTD_isError(ZSTD_decompressStream(context.get(), &out, &in)));
			}
		}
		data.resize(out.pos);
	}
	else
	{
		data = payload(0x4000000);
		if (kind == "zero") std::fill(data.begin(), data.end(), 0);
		if (kind == "mixed")
		{
			for (usz i = 4096; i < data.size(); i += 8192)
				std::copy_n(data.data(), 4096, data.data() + i);
		}
	}
	ASSERT_FALSE(data.empty());
	const auto start = std::chrono::steady_clock::now();
	write(data);
	const auto saved = std::chrono::steady_clock::now();
	roundtrip(data);
	const auto loaded = std::chrono::steady_clock::now();
	const double save_seconds = std::chrono::duration<double>(saved - start).count();
	const double load_seconds = std::chrono::duration<double>(loaded - saved).count();
	std::cout << "BENCHMARK {\"kind\":\"" << kind << "\",\"input_bytes\":" << data.size()
		<< ",\"file_bytes\":" << fs::file(path).size() << ",\"save_seconds\":" << save_seconds
		<< ",\"load_seconds\":" << load_seconds;
#ifndef _WIN32
	struct rusage usage{};
	getrusage(RUSAGE_SELF, &usage);
	std::cout << ",\"peak_rss\":" << usage.ru_maxrss;
#endif
	std::cout << "}\n";
}
