/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-misc.c: Miscellaneous settings and configuration file handling.
 *
 * Authors:
 *      Alex Graveley (alex@ximian.com)
 *
 * soup_base64_encode() written by Chris Blizzard <blizzard@mozilla.org>,
 * and is Copyright (C) 1998 Free Software Foundation.
 *
 * All else Copyright (C) 2000, Helix Code, Inc.
 */

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "soup-misc.h"
#include "soup-private.h"

gboolean soup_initialized = FALSE;

static guint max_connections = 0;

static SoupContext *proxy_context = NULL;

static SoupSecurityPolicy ssl_security_level = SOUP_SECURITY_DOMESTIC;

/**
 * soup_set_proxy:
 * @context: a %SoupContext to use as the proxy context for all outgoing
 * connections.
 *
 * Use @context as the %SoupContext to connect to instead of the actual
 * destination specified in a SoupMessage. Messages will be routed through the
 * proxy host on their way to the actual specified destination. The URL for this
 * context should be of the form:
 * 	[http|https|socks4|socks5]://<USERNAME>:<PASSWORD>@<PROXYHOST>
 */
void
soup_set_proxy (SoupContext *context)
{
	if (proxy_context)
		soup_context_unref (proxy_context);

	proxy_context = context;

	if (proxy_context)
		soup_context_ref (proxy_context);
}

/**
 * soup_get_proxy:
 *
 * Get the current proxy %SoupContext.
 *
 * Return value: the current proxy context.
 */
SoupContext *
soup_get_proxy (void)
{
	return proxy_context;
}

/**
 * soup_set_connection_limit:
 * @max_conn: the number of connections.
 *
 * Set the maximum concurrent connection limit for outgoing requests.
 */
void
soup_set_connection_limit (guint max_conn)
{
	max_connections = max_conn;
}

/**
 * soup_get_connection_limit:
 *
 * Return value: The maximum concurrent connection limit for outgoing requests.
 */
guint
soup_get_connection_limit (void)
{
	return max_connections;
}

/**
 * soup_set_security_policy:
 * @policy: the %SoupSecurityPolicy to use.
 *
 * Set the security policy for all secure SSL connections. The security policy
 * dictates which algorithms and encryption levels can be used in order to
 * conform to your country's security legislation.
 */
void
soup_set_security_policy (SoupSecurityPolicy policy)
{
	ssl_security_level = policy;
}

/**
 * soup_get_security_policy:
 *
 * Return value: The security policy to use for secure SSL connections.
 */
SoupSecurityPolicy
soup_get_security_policy (void)
{
	return ssl_security_level;
}


guint
soup_str_case_hash (gconstpointer key)
{
	const char *p = key;
	guint h = toupper(*p);

	if (h)
		for (p += 1; *p != '\0'; p++)
			h = (h << 5) - h + toupper(*p);

	return h;
}

gboolean
soup_str_case_equal (gconstpointer v1,
		     gconstpointer v2)
{
	const gchar *string1 = v1;
	const gchar *string2 = v2;

	return g_strcasecmp (string1, string2) == 0;
}

gint
soup_substring_index (gchar *str, gint len, gchar *substr)
{
	int i, sublen = strlen (substr);

	for (i = 0; i <= len - sublen; ++i)
		if (str[i] == substr[0])
			if (memcmp (&str[i], substr, sublen) == 0)
				return i;

	return -1;
}

const char base64_alphabet[65] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";

/**
 * soup_base64_encode:
 * @text: the binary data to encode.
 * @inlen: the length of @text.
 *
 * Encode a sequence of binary data into it's Base-64 stringified
 * representation.
 *
 * Return value: The Base-64 encoded string representing @text.
 */
gchar *
soup_base64_encode (const gchar *text, gint inlen)
{
	char *buffer = NULL;
	char *point = NULL;
	int outlen = 0;

	/* check our args */
	if (text == NULL)
		return NULL;

	/* Use 'buffer' to store the output. Work out how big it should be...
	 * This must be a multiple of 4 bytes */

	/* check our arg...avoid a pesky FPE */
	if (inlen == 0) {
		buffer = g_malloc (sizeof(char));
		buffer[0] = '\0';
		return buffer;
	}

	outlen = (inlen*4)/3;
	if ((inlen % 3) > 0) /* got to pad */
		outlen += 4 - (inlen % 3);

	buffer = g_malloc (outlen + 1); /* +1 for the \0 */
	memset (buffer, 0, outlen + 1); /* initialize to zero */

	/* now do the main stage of conversion, 3 bytes at a time,
	 * leave the trailing bytes (if there are any) for later */

	for (point=buffer; inlen>=3; inlen-=3, text+=3) {
		*(point++) = base64_alphabet [*text>>2];
		*(point++) = base64_alphabet [(*text<<4 & 0x30) |
					     *(text+1)>>4];
		*(point++) = base64_alphabet [(*(text+1)<<2 & 0x3c) |
					     *(text+2)>>6];
		*(point++) = base64_alphabet [*(text+2) & 0x3f];
	}

	/* Now deal with the trailing bytes */
	if (inlen) {
		/* We always have one trailing byte */
		*(point++) = base64_alphabet [*text>>2];
		*(point++) = base64_alphabet [(*text<<4 & 0x30) |
					     (inlen==2?*(text+1)>>4:0)];
		*(point++) = (inlen == 1 ?
			      '=' :
			      base64_alphabet [*(text+1)<<2 & 0x3c]);
		*(point++) = '=';
	}

	*point = '\0';

	return buffer;
}

#define ALLOW_UNLESS_DENIED TRUE
#define DENY_UNLESS_ALLOWED FALSE

static gboolean allow_policy = ALLOW_UNLESS_DENIED;
static GSList *allow_tokens = NULL;
static GSList *deny_tokens = NULL;

static void
soup_config_connection_limit (gchar *key, gchar *value)
{
	soup_set_connection_limit (MAX (atoi (value), 0));
}

static void
soup_config_proxy_uri (gchar *key, gchar *value)
{
	SoupContext *con = soup_context_get (value);
	if (con) soup_set_proxy (con);
}

static void
soup_config_security_policy (gchar *key, gchar *value)
{
	switch (toupper (value [0])) {
	case 'D':
		if (!g_strcasecmp (&value [1], "OMESTIC"))
			soup_set_security_policy (SOUP_SECURITY_DOMESTIC);
		break;
	case 'E':
		if (!g_strcasecmp (&value [1], "XPORT"))
			soup_set_security_policy (SOUP_SECURITY_EXPORT);
		break;
	case 'F':
		if (!g_strcasecmp (&value [1], "RANCE"))
			soup_set_security_policy (SOUP_SECURITY_FRANCE);
		break;
	}
}

static void
soup_config_ssl_ca_file (gchar *key, gchar *value)
{
	soup_set_ssl_ca_file (value);
}

static void
soup_config_ssl_ca_directory (gchar *key, gchar *value)
{
	soup_set_ssl_ca_dir (value);
}

static void
soup_config_ssl_certificate (gchar *key, gchar *value)
{
	gint idx;

	idx = strcspn (value, " \t");
	if (!idx) return;
	
	value [idx] = '\0';

	idx += strspn (value + idx + 1, " \t");
	if (!idx) return;

	soup_set_ssl_cert_files (value, value + idx);
}

typedef void (*SoupConfigFunc) (gchar *key, gchar *value);

struct SoupConfigFuncs {
	gchar          *key;
	SoupConfigFunc  func;
} soup_config_funcs [] = {
	{ "connection-limit", soup_config_connection_limit },
	{ "proxy-uri",        soup_config_proxy_uri },
	{ "proxy-url",        soup_config_proxy_uri },
	{ "security-policy",  soup_config_security_policy },
	{ "ssl-ca-file",      soup_config_ssl_ca_file },
	{ "ssl-ca-directory", soup_config_ssl_ca_directory },
	{ "ssl-certificate",  soup_config_ssl_certificate },
	{ NULL }
};

static void
soup_config_reset_allow_deny (void)
{
	GSList *iter;

	for (iter = allow_tokens; iter; iter = iter->next) g_free (iter->data);
	for (iter = deny_tokens; iter; iter = iter->next) g_free (iter->data);

	g_slist_free (allow_tokens);
	g_slist_free (deny_tokens);

	allow_tokens = deny_tokens = NULL;
}

static gboolean
soup_config_allow_deny (gchar *key)
{
	GSList **list;
	gchar **iter, **split;

	key = g_strchomp (key);

	if (!g_strncasecmp (key, "allow", 5)) list = &allow_tokens;
	else if (!g_strncasecmp (key, "deny", 4)) list = &deny_tokens;
	else return FALSE;

	iter = split = g_strsplit (key, " ", 0);
	if (!split || !split [1]) return TRUE;

	while (*(++iter)) {
		if (!g_strcasecmp (iter [0], "all")) {
			GSList *iter;
			allow_policy = (*list == allow_tokens);
			for (iter = *list; iter; iter = iter->next)
				g_free (iter->data);
			g_slist_free (*list);
			*list = NULL;
			*list = g_slist_prepend (*list, NULL);
			break;
		}

		*list = g_slist_prepend (*list, g_strdup (iter [0]));
	}

	g_strfreev (split);
	return TRUE;
}

static gboolean
soup_config_token_allowed (gchar *key)
{
	gboolean allow;
	GSList *list;

	list = (allow_policy == ALLOW_UNLESS_DENIED) ? deny_tokens:allow_tokens;
	allow = (allow_policy == ALLOW_UNLESS_DENIED) ? TRUE : FALSE;

	if (!list) return allow;

	for (; list; list = list->next)
		if (!list->data ||
		    !g_strncasecmp (key,
				    (gchar *) list->data,
				    strlen ((gchar *) list->data)))
			return !allow;

	return allow;
}

static void
soup_load_config_internal (gchar *config_file, gboolean admin)
{
	struct SoupConfigFuncs *funcs;
	FILE *cfg;
	char buf[128];

	cfg = fopen (config_file, "r");
	if (!cfg) return;

	if (admin) soup_config_reset_allow_deny();

	while (fgets (buf, sizeof (buf), cfg)) {
		char *key, *value, *iter, *iter2, **split;

		iter = g_strstrip (buf);
		if (!*iter || *iter == '#') continue;

		iter2 = strchr (iter, '#');
		if (iter2) *iter2 = '\0';

		if (admin && soup_config_allow_deny (iter)) continue;

		if (!admin && !soup_config_token_allowed (iter)) {
			g_warning ("Configuration item \"%s\" in file \"%s\" "
				   "disallowed by system configuration.\n",
				   iter,
				   config_file);
			continue;
		}

		split = g_strsplit (g_strchomp (iter), "=", 2);

		if (!split) continue;
		if (!split[1] || split[2]) {
			g_strfreev (split);
			continue;
		}

		key = g_strchomp (split[0]);
		value = g_strchug (split[1]);

		for (funcs = soup_config_funcs; funcs && funcs->key; funcs++)
			if (!g_strcasecmp (key, funcs->key)) {
				funcs->func (key, value);
				break;
			}

		g_strfreev (split);
	}
}

/**
 * soup_load_config:
 * @config_file: The file to load configuration from. If NULL, load from .souprc
 * in user's home directory.
 *
 * Load the Soup configuration from file. First attempt to load the system
 * configuration from SYSCONFDIR/souprc, then from either the config file name
 * passed in config_file, or from .souprc in the user's home directory.
 *
 * The first time a message is sent using Soup, the configuration is loaded from
 * the system souprc file, and the user's souprc file.
 *
 * soup_load_config can be called multiple times. Each time settings will be
 * reset and reread from scratch.
 */
void
soup_load_config (gchar *config_file)
{
	/* Reset values */
	if (soup_initialized) {
		soup_set_proxy (NULL);
		soup_set_connection_limit (0);
		soup_set_security_policy (SOUP_SECURITY_DOMESTIC);
	}

#ifdef SYSCONFDIR
	/* Load system global config */
	soup_load_config_internal (SYSCONFDIR G_DIR_SEPARATOR_S "souprc",
				   TRUE);
#endif

	/* Load requested file or user local config */
	if (!config_file) {
		gchar *dfile = g_strconcat (g_get_home_dir(),
					    G_DIR_SEPARATOR_S ".souprc",
					    NULL);
		soup_load_config_internal (dfile, FALSE);
		g_free (dfile);
	} else
		soup_load_config_internal (config_file, FALSE);

	soup_initialized = TRUE;
}

/**
 * soup_shutdown:
 *
 * Shut down the Soup engine.
 *
 * The pending message queue is flushed by calling %soup_message_cancel on all
 * active requests.
 */
void
soup_shutdown ()
{
	soup_queue_shutdown ();
}

/**
 * soup_set_ca_file:
 * @ca_file: the path to a CA file
 *
 * Specify a file containing CA certificates to be used to verify
 * peers.
 */
void
soup_set_ssl_ca_file (gchar *ca_file)
{
	putenv (g_strdup_printf ("HTTPS_CA_FILE=%s", ca_file));
}

/**
 * soup_set_ca_dir
 * @ca_dir: the directory containing CA certificate files
 *
 * Specify a directory containing CA certificates to be used to verify
 * peers.
 */
void
soup_set_ssl_ca_dir (gchar *ca_dir)
{
	putenv (g_strdup_printf ("HTTPS_CA_DIR=%s", ca_dir));
}

/**
 * soup_set_ssl_cert_files
 * @cert_file: the file containing the SSL client certificate
 * @key_file: the file containing the SSL private key
 *
 * Specify a SSL client certificate to be used for client
 * authentication with the SOAP server
 */
void
soup_set_ssl_cert_files (gchar *cert_file, gchar *key_file)
{
	putenv (g_strdup_printf ("HTTPS_CERT_FILE=%s", cert_file));
	putenv (g_strdup_printf ("HTTPS_KEY_FILE=%s", key_file));
}
