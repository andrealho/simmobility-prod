/* Copyright Singapore-MIT Alliance for Research and Technology */

#pragma once

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>

namespace unit_tests
{

/**
 * Unit Tests for the FixedDelayed class
 * \author Seth N. Hetu
 */
class FixedDelayedUnitTests : public CppUnit::TestFixture
{
public:
	///Test simple setting and retrieving
	void test_FixedDelayed_simple_set_get();

	///Test retrieving a value too early.
	void test_FixedDelayed_bad_retrieve();

	///Test retrieving a value directly on the target time
	void test_FixedDelayed_exact_retrieve();

	///Test a zero-waiting-time retrieve
	void test_FixedDelayed_zero_retrieve();

	///Several common consistency checks
	void test_FixedDelayed_sanity_checks();

	///Ensure memory isn't wrongly deleted
	void test_FixedDelayed_false_delete();

	///Ensure memory isn't leaked
	void test_FixedDelayed_skipped_delete();





private:
    CPPUNIT_TEST_SUITE(FixedDelayedUnitTests);
        CPPUNIT_TEST(test_FixedDelayed_simple_set_get);
        CPPUNIT_TEST(test_FixedDelayed_bad_retrieve);
        CPPUNIT_TEST(test_FixedDelayed_exact_retrieve);
        CPPUNIT_TEST(test_FixedDelayed_zero_retrieve);
        CPPUNIT_TEST(test_FixedDelayed_sanity_checks);
        CPPUNIT_TEST(test_FixedDelayed_false_delete);
        CPPUNIT_TEST(test_FixedDelayed_skipped_delete);
    CPPUNIT_TEST_SUITE_END();
};

}
