/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef UDS_ERRORS_H
#define UDS_ERRORS_H

#include <linux/compiler.h>
#include <linux/types.h>
#ifndef __KERNEL__
#include <errno.h>
#endif /*  __KERNEL__ */

/* Custom error codes and error-related utilities */
#define VDO_SUCCESS 0

#ifdef TEST_INTERNAL
/* If you add a value to this enum be sure to add a corresponding error description in errors.c! */

#endif
/* Valid status codes for internal UDS functions. */
enum uds_status_codes {
	/* Successful return */
	UDS_SUCCESS = VDO_SUCCESS,
	/* Used as a base value for reporting internal errors */
	UDS_ERROR_CODE_BASE = 1024,
	/* Index overflow */
	UDS_OVERFLOW = UDS_ERROR_CODE_BASE,
	/* Invalid argument passed to internal routine */
	UDS_INVALID_ARGUMENT,
	/* UDS data structures are in an invalid state */
	UDS_BAD_STATE,
	/* Attempt to enter the same name into an internal structure twice */
	UDS_DUPLICATE_NAME,
	/* An assertion failed */
	UDS_ASSERTION_FAILED,
	/* A request has been queued for later processing (not an error) */
	UDS_QUEUED,
	/* This error range has already been registered */
	UDS_ALREADY_REGISTERED,
	/* Attempt to read or write data outside the valid range */
	UDS_OUT_OF_RANGE,
	/* The index session is disabled */
	UDS_DISABLED,
	/* The index configuration or volume format is no longer supported */
	UDS_UNSUPPORTED_VERSION,
	/* Some index structure is corrupt */
	UDS_CORRUPT_DATA,
	/* No index state found */
	UDS_NO_INDEX,
	/* Attempt to access incomplete index save data */
	UDS_INDEX_NOT_SAVED_CLEANLY,
#if defined(TEST_INTERNAL) || !defined(__KERNEL__)
	/* No directory was found where one was expected */
	UDS_NO_DIRECTORY,
	/* Could not load modules */
	UDS_EMODULE_LOAD,
	/* Unknown error */
	UDS_UNKNOWN_ERROR,
#endif /*  Test_INTERNAL || ! __KERNEL__ */
	/* One more than the last UDS_INTERNAL error code */
	UDS_ERROR_CODE_LAST,
	/* One more than the last error this block will ever use */
	UDS_ERROR_CODE_BLOCK_END = UDS_ERROR_CODE_BASE + 440,
};

enum {
	VDO_MAX_ERROR_NAME_SIZE = 80,
	VDO_MAX_ERROR_MESSAGE_SIZE = 128,
};

struct error_info {
	const char *name;
	const char *message;
};

const char * __must_check uds_string_error(int errnum, char *buf, size_t buflen);

const char *uds_string_error_name(int errnum, char *buf, size_t buflen);

int uds_status_to_errno(int error);

int uds_register_error_block(const char *block_name, int first_error,
			     int last_reserved_error, const struct error_info *infos,
			     size_t info_size);

#endif /* UDS_ERRORS_H */
