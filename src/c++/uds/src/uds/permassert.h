/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef PERMASSERT_H
#define PERMASSERT_H

#ifndef __KERNEL__
#include <linux/build_bug.h>
#endif
#include <linux/compiler.h>

#include "errors.h"

/* Utilities for asserting that certain conditions are met */

#define STRINGIFY(X) #X

/*
 * A hack to apply the "warn if unused" attribute to an integral expression.
 *
 * Since GCC doesn't propagate the warn_unused_result attribute to conditional expressions
 * incorporating calls to functions with that attribute, this function can be used to wrap such an
 * expression. With optimization enabled, this function contributes no additional instructions, but
 * the warn_unused_result attribute still applies to the code calling it.
 */
static inline int __must_check vdo_must_use(int value)
{
	return value;
}

/* Assert that an expression is true and return an error if it is not. */
#define VDO_ASSERT(expr, ...) vdo_must_use(__VDO_ASSERT(expr, __VA_ARGS__))

/* Log a message if the expression is not true. */
#define VDO_ASSERT_LOG_ONLY(expr, ...) __VDO_ASSERT(expr, __VA_ARGS__)

/* For use by UDS */
#define ASSERT(expr, ...) VDO_ASSERT(expr, __VA_ARGS__)
#define ASSERT_LOG_ONLY(expr, ...) __VDO_ASSERT(expr, __VA_ARGS__)

#define __VDO_ASSERT(expr, ...)				      \
	(likely(expr) ? VDO_SUCCESS			      \
		      : uds_assertion_failed(STRINGIFY(expr), __FILE__, __LINE__, __VA_ARGS__))

/* Log an assertion failure message. */
int uds_assertion_failed(const char *expression_string, const char *file_name,
			 int line_number, const char *format, ...)
	__printf(4, 5);

#ifndef __KERNEL__
#define STATIC_ASSERT(expr)	     \
	do {			     \
		switch (0) {	     \
		case 0:		     \
			;	     \
			fallthrough; \
		case expr:	     \
			;	     \
			fallthrough; \
		default:	     \
			break;	     \
		}		     \
	} while (0)

#define STATIC_ASSERT_SIZEOF(type, expected_size) STATIC_ASSERT(sizeof(type) == (expected_size))

/* Set whether or not to exit on an assertion failure, for tests. */
bool set_exit_on_assertion_failure(bool should_exit);

#endif /* not __KERNEL__ */
#endif /* PERMASSERT_H */
