/* Copyright 1988,1990,1993,1994 by Paul Vixie
 * All rights reserved
 */

/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1997,2000 by Internet Software Consortium, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The following states are used by load_env(), traversed in order: */
enum env_state {
	NAMEI,		/* First char of NAME, may be quote */
	NAME,		/* Subsequent chars of NAME */
	EQ1,		/* After end of name, looking for '=' sign */
	EQ2,		/* After '=', skipping whitespace */
	VALUEI,		/* First char of VALUE, may be quote */
	VALUE,		/* Subsequent chars of VALUE */
	FINI,		/* All done, skipping trailing whitespace */
	ERROR,		/* Error */
};

/* return	false = not an env setting
 *		true = was an env setting
 */
extern bool load_env(char *envstr) {
	enum env_state state;
	char quotechar, *c, *str;
	char name[131072], val[131072];

	bzero(name, sizeof name);
	bzero(val, sizeof val);
	str = name;
	state = NAMEI;
	quotechar = '\0';
	c = envstr;
	while (state != ERROR && *c) {
		switch (state) {
		case NAMEI:
		case VALUEI:
			if (*c == '\'' || *c == '"')
				quotechar = *c++;
			state++;
			/* FALLTHROUGH */
		case NAME:
		case VALUE:
			if (quotechar) {
				if (*c == quotechar) {
					state++;
					c++;
					break;
				}
				if (state == NAME && *c == '=') {
					state = ERROR;
					break;
				}
			} else {
				if (state == NAME) {
					if (isspace((unsigned char)*c)) {
						c++;
						state++;
						break;
					}
					if (*c == '=') {
						state++;
						break;
					}
				}
			}
			*str++ = *c++;
			break;

		case EQ1:
			if (*c == '=') {
				state++;
				str = val;
				quotechar = '\0';
			} else {
				if (!isspace((unsigned char)*c))
					state = ERROR;
			}
			c++;
			break;

		case EQ2:
		case FINI:
			if (isspace((unsigned char)*c))
				c++;
			else
				state++;
			break;

		default:
			abort();
		}
	}
	if (state != FINI && !(state == VALUE && !quotechar)) {
		return false;
	}
	if (state == VALUE) {
		/* End of unquoted value: trim trailing whitespace */
		c = val + strlen(val);
		while (c > val && isspace((unsigned char)c[-1]))
			*(--c) = '\0';
	}

	/* 2 fields from parser; looks like an env setting */

	return true;
}
