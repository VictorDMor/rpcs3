#include "Utilities/JIT.h"
#include "Utilities/Thread.h"

#include <gtest/gtest.h>
#include <chrono>

#ifdef LLVM_AVAILABLE
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

extern atomic_t<bool> g_headless;

class jit_object_cache : public testing::Test
{
protected:
	bool was_headless = false;
	std::string name;

	void SetUp() override
	{
		was_headless = g_headless.exchange(true);
		name = "object_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		jit_runtime::initialize();
	}

	void TearDown() override
	{
		jit_runtime::finalize();
		g_headless = was_headless;
	}

	std::unique_ptr<llvm::Module> make_module(jit_compiler& compiler, u64 value, bool external = false)
	{
		auto& context = compiler.get_context();
		auto module = std::make_unique<llvm::Module>(name, context);
		module->setTargetTriple(llvm::Triple(jit_compiler::triple2()));
		module->setDataLayout(compiler.get_engine().getTargetMachine()->createDataLayout());
		llvm::IRBuilder<> builder(context);
		auto type = llvm::FunctionType::get(builder.getInt64Ty(), false);
		auto function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "cache_test", module.get());
		builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
		llvm::Value* result = builder.getInt64(value);
		if (external)
		{
			result = builder.CreateCall(module->getOrInsertFunction("cache_external", type));
		}
		builder.CreateRet(result);
		return module;
	}

	u64 compile_and_call(u64 value, bool cache = true, u64 external = 0, u32 flags = 0)
	{
		u64 result = 0;
		named_thread caller("Object cache test"sv, [&]()
		{
			jit_compiler compiler{{}, {}, flags};
			if (external)
			{
				compiler.update_global_mapping("cache_external", external);
			}
			std::string error;
			{
				jit_write_guard guard;
				ASSERT_TRUE(compiler.try_compile(make_module(compiler, value, external != 0), cache, error)) << error;
			}
			const auto function = reinterpret_cast<u64(*)()>(compiler.get("cache_test"));
			ASSERT_NE(function, nullptr);
			result = function();
		});
		caller();
		EXPECT_EQ(caller, thread_state::finished);
		return result;
	}
};

TEST_F(jit_object_cache, ReusesRelocatableObjectAfterJitReset)
{
	const auto before = jit_compiler::get_object_cache_stats();
	ASSERT_EQ(compile_and_call(42), 42);
	const auto cold = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(cold.misses - before.misses, 1);
	EXPECT_EQ(cold.hits, before.hits);
	jit_runtime::finalize();
	ASSERT_EQ(compile_and_call(42), 42);
	const auto warm = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(warm.hits - cold.hits, 1);
	EXPECT_EQ(warm.misses, cold.misses);
}

TEST_F(jit_object_cache, ChangedIrCannotReuseOldObject)
{
	ASSERT_EQ(compile_and_call(42), 42);
	const auto before = jit_compiler::get_object_cache_stats();
	ASSERT_EQ(compile_and_call(99), 99);
	const auto after = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(after.misses - before.misses, 1);
	EXPECT_EQ(after.hits, before.hits);
}

TEST_F(jit_object_cache, ChangedTargetCannotReuseOldObject)
{
	ASSERT_EQ(compile_and_call(42), 42);
	const auto before = jit_compiler::get_object_cache_stats();
	ASSERT_EQ(compile_and_call(42, true, 0, 0x2), 42);
	const auto after = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(after.misses - before.misses, 1);
	EXPECT_EQ(after.hits, before.hits);
}

TEST_F(jit_object_cache, CachedObjectRelinksExternalSymbols)
{
	const auto first = +[]() -> u64 { return 12; };
	const auto second = +[]() -> u64 { return 34; };
	ASSERT_EQ(compile_and_call(0, true, reinterpret_cast<u64>(first)), 12);
	const auto before = jit_compiler::get_object_cache_stats();
	jit_runtime::finalize();
	ASSERT_EQ(compile_and_call(0, true, reinterpret_cast<u64>(second)), 34);
	const auto after = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(after.hits - before.hits, 1);
	EXPECT_EQ(after.misses, before.misses);
}

TEST_F(jit_object_cache, DisabledCacheDoesNotPopulateOrReadCache)
{
	const auto before = jit_compiler::get_object_cache_stats();
	ASSERT_EQ(compile_and_call(42, false), 42);
	ASSERT_EQ(compile_and_call(42, false), 42);
	const auto after = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(after.hits, before.hits);
	EXPECT_EQ(after.misses, before.misses);
	EXPECT_EQ(after.bytes, before.bytes);
}

TEST_F(jit_object_cache, ConcurrentCompilersKeepCachedObjectsImmutable)
{
	ASSERT_EQ(compile_and_call(42), 42);
	const auto before = jit_compiler::get_object_cache_stats();
	named_thread_group workers("Concurrent object cache test"sv, 8, [&]()
	{
		return compile_and_call(42);
	});
	for (u32 i = 0; i < workers.size(); i++)
	{
		EXPECT_EQ(workers[i], 42);
	}
	const auto after = jit_compiler::get_object_cache_stats();
	EXPECT_EQ(after.hits - before.hits, 8);
	EXPECT_EQ(after.misses, before.misses);
	EXPECT_EQ(after.bytes, before.bytes);
}
#endif
