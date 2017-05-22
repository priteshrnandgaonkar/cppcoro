///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////

#include <cppcoro/task.hpp>
#include <cppcoro/single_consumer_event.hpp>

#include "counted.hpp"

#include "doctest/doctest.h"

TEST_SUITE_BEGIN("task");

TEST_CASE("default constructed task")
{
	cppcoro::task<> t;

	CHECK(t.is_ready());

	SUBCASE("throws broken_promise when awaited")
	{
		auto op = t.operator co_await();
		CHECK(op.await_ready());
		CHECK_THROWS_AS(op.await_resume(), const cppcoro::broken_promise&);
	}
}

TEST_CASE("co_await synchronously completing task")
{
	auto doNothingAsync = []() -> cppcoro::task<>
	{
		co_return;
	};

	auto task = doNothingAsync();

	CHECK(task.is_ready());

	bool ok = false;
	auto test = [&]() -> cppcoro::task<>
	{
		co_await task;
		ok = true;
	};

	test();

	CHECK(ok);
}

TEST_CASE("task of move-only type by value")
{
	// unique_ptr is move-only type.
	auto getIntPtrAsync = []() -> cppcoro::task<std::unique_ptr<int>>
	{
		co_return std::make_unique<int>(123);
	};

	SUBCASE("co_await temporary")
	{
		auto test = [&]() -> cppcoro::task<>
		{
			auto intPtr = co_await getIntPtrAsync();
			REQUIRE(intPtr);
			CHECK(*intPtr == 123);
		};

		test();
	}

	SUBCASE("co_await lvalue reference")
	{
		auto test = [&]() -> cppcoro::task<>
		{
			// co_await yields l-value reference if task is l-value
			auto intPtrTask = getIntPtrAsync();
			auto& intPtr = co_await intPtrTask;
			REQUIRE(intPtr);
			CHECK(*intPtr == 123);
		};

		test();
	}

	SUBCASE("co_await rvalue reference")
	{
		auto test = [&]() -> cppcoro::task<>
		{
			// Returns r-value reference if task is r-value
			auto intPtrTask = getIntPtrAsync();
			auto intPtr = co_await std::move(intPtrTask);
			REQUIRE(intPtr);
			CHECK(*intPtr == 123);
		};

		test();
	}
}

TEST_CASE("task of reference type")
{
	int value = 0;
	auto getRefAsync = [&]() -> cppcoro::task<int&>
	{
		co_return value;
	};

	auto test = [&]() -> cppcoro::task<>
	{
		// Await r-value task results in l-value reference
		decltype(auto) result = co_await getRefAsync();
		CHECK(&result == &value);

		// Await l-value task results in l-value reference
		auto getRefTask = getRefAsync();
		decltype(auto) result2 = co_await getRefTask;
		CHECK(&result2 == &value);
	};

	auto task = test();
	CHECK(task.is_ready());
}

TEST_CASE("task of value-type moves into promise if passed rvalue reference")
{
	counted::reset_counts();

	auto f = []() -> cppcoro::task<counted>
	{
		co_return counted{};
	};

	CHECK(counted::active_count() == 0);

	{
		auto t = f();
		CHECK(counted::default_construction_count == 1);
		CHECK(counted::copy_construction_count == 0);
		CHECK(counted::move_construction_count == 1);
		CHECK(counted::destruction_count == 1);
		CHECK(counted::active_count() == 1);

		// Moving task doesn't move/copy result.
		auto t2 = std::move(t);
		CHECK(counted::default_construction_count == 1);
		CHECK(counted::copy_construction_count == 0);
		CHECK(counted::move_construction_count == 1);
		CHECK(counted::destruction_count == 1);
		CHECK(counted::active_count() == 1);
	}

	CHECK(counted::active_count() == 0);
}

TEST_CASE("task of value-type copies into promise if passed lvalue reference")
{
	counted::reset_counts();

	auto f = []() -> cppcoro::task<counted>
	{
		counted temp;

		// Should be calling copy-constructor here since <promise>.return_value()
		// is being passed an l-value reference.
		co_return temp;
	};

	CHECK(counted::active_count() == 0);

	{
		auto t = f();
		CHECK(counted::default_construction_count == 1);
		CHECK(counted::copy_construction_count == 1);
		CHECK(counted::move_construction_count == 0);
		CHECK(counted::destruction_count == 1);
		CHECK(counted::active_count() == 1);

		// Moving the task doesn't move/copy the result
		auto t2 = std::move(t);
		CHECK(counted::default_construction_count == 1);
		CHECK(counted::copy_construction_count == 1);
		CHECK(counted::move_construction_count == 0);
		CHECK(counted::destruction_count == 1);
		CHECK(counted::active_count() == 1);
	}

	CHECK(counted::active_count() == 0);
}

TEST_CASE("co_await chain of async completions")
{
	cppcoro::single_consumer_event event;
	bool reachedPointA = false;
	bool reachedPointB = false;
	auto async1 = [&]() -> cppcoro::task<int>
	{
		reachedPointA = true;
		co_await event;
		reachedPointB = true;
		co_return 1;
	};

	bool reachedPointC = false;
	bool reachedPointD = false;
	auto async2 = [&]() -> cppcoro::task<int>
	{
		reachedPointC = true;
		int result = co_await async1();
		reachedPointD = true;
		co_return result;
	};

	auto task = async2();

	CHECK(!task.is_ready());
	CHECK(reachedPointA);
	CHECK(!reachedPointB);
	CHECK(reachedPointC);
	CHECK(!reachedPointD);

	event.set();

	CHECK(task.is_ready());
	CHECK(reachedPointB);
	CHECK(reachedPointD);

	[](cppcoro::task<int> t) -> cppcoro::task<>
	{
		int value = co_await t;
		CHECK(value == 1);
	}(std::move(task));
}

TEST_CASE("awaiting default-constructed task throws broken_promise")
{
	[]() -> cppcoro::task<>
	{
		cppcoro::task<> broken;
		CHECK_THROWS_AS(co_await broken, const cppcoro::broken_promise&);
	}();
}

TEST_CASE("awaiting task that completes with exception")
{
	class X {};

	auto run = [](bool doThrow = true) -> cppcoro::task<>
	{
		if (doThrow) throw X{};
		co_return;
	};

	auto t = run();
	CHECK(t.is_ready());

	auto consumeT = [&]() -> cppcoro::task<>
	{
		SUBCASE("co_await task rethrows exception")
		{
			CHECK_THROWS_AS(co_await t, const X&);
		}

		SUBCASE("co_await task.when_ready() doesn't rethrow exception")
		{
			CHECK_NOTHROW(co_await t.when_ready());
		}
	};

	auto consumer = consumeT();
	CHECK(consumer.is_ready());
}

TEST_SUITE_END();