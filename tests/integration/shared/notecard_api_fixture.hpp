#pragma once
/// @file notecard_api_fixture.hpp
/// Global Api accessor for shared integration tests.
///
/// Each environment (serial, I2C, softcard) defines g_api in its main.cpp
/// during PTR_BOARD_INIT. Shared test cases call notecard_api() to get it.

#include <note/api.hpp>

// Global Api instance — set by each environment's board init.
extern note::Api<>* g_api;

/// Get the global Api reference. Asserts that it was initialized.
inline note::Api<>& notecard_api() { return *g_api; }
