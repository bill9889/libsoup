/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-queue.c: Asyncronous Callback-based SOAP Request Queue.
 *
 * Authors:
 *      Alex Graveley (alex@helixcode.com)
 *
 * Copyright (C) 2000, Helix Code, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#include "soup-transfer.h"
#include "soup-private.h"

#undef DUMP

#ifdef DUMP
static void
DUMP_READ (guchar *data, gint bytes_read) 
{
	gchar *buf = alloca (bytes_read + 1);
	memcpy (buf, data, bytes_read);
	buf[bytes_read] = '\0';
	
	g_warning ("READ %d\n----------\n%s\n----------\n", bytes_read, buf);
}
static void
DUMP_WRITE (guchar *data, gint bytes_written) 
{
	gchar *buf = alloca (bytes_written + 1);
	memcpy (buf, data, bytes_written);
	buf[bytes_written] = '\0';

	g_warning ("WRITE %d\n----------\n%s\n----------\n", bytes_written,buf);
}
#else
#  define DUMP_READ(x,y)
#  define DUMP_WRITE(x,y)
#endif

typedef struct {
	/* 
	 * Length remaining to be downloaded of the current chunk data. 
	 */
	guint  len;

	/* 
	 * Index into the recv buffer where this chunk's data begins.
	 * 0 if overwrite chunks is active.
	 */
	guint  idx;
} SoupTransferChunkState;

typedef struct {
	GIOChannel            *channel;
	guint                  read_tag;
	guint                  err_tag;

	/*
	 * If TRUE, a callback has been issued which references recv_buf.
	 * If the transfer is cancelled before a reference exists, the contents
	 * of recv_buf are free'd.
	 */
	gboolean               callback_issued;

	gboolean               processing;

	GByteArray            *recv_buf;
	guint                  header_len;

	gboolean               overwrite_chunks;

	SoupTransferEncoding   encoding;
	guint                  content_length;
	SoupTransferChunkState chunk_state;

	SoupReadHeadersDoneFn  headers_done_cb;
	SoupReadChunkFn        read_chunk_cb;
	SoupReadDoneFn         read_done_cb;
	SoupReadErrorFn        error_cb;
	gpointer               user_data;
} SoupReader;

typedef struct {
	GIOChannel             *channel;
	guint                   write_tag;
	guint                   err_tag;

	gboolean                processing;

	SoupTransferEncoding    encoding;
	GByteArray             *write_buf;

	guint                   header_len;
	gboolean                headers_done;
	gint                    chunk_cnt;

	SoupWriteHeadersDoneFn  headers_done_cb;
	SoupWriteChunkFn        write_chunk_cb;
	SoupWriteDoneFn         write_done_cb;
	SoupWriteErrorFn        error_cb;
	gpointer                user_data;
} SoupWriter;

#define IGNORE_CANCEL(t) (t)->processing = TRUE;
#define UNIGNORE_CANCEL(t) (t)->processing = FALSE;

void
soup_transfer_read_cancel (guint tag)
{
	SoupReader *r = GINT_TO_POINTER (tag);

	if (r->processing) return;

	g_source_remove (r->read_tag);
	g_source_remove (r->err_tag);

	g_byte_array_free (r->recv_buf, r->callback_issued ? FALSE : TRUE);

	g_free (r);
}

void 
soup_transfer_read_set_callbacks (guint                   tag,
				  SoupReadHeadersDoneFn   headers_done_cb,
				  SoupReadChunkFn         read_chunk_cb,
				  SoupReadDoneFn          read_done_cb,
				  SoupReadErrorFn         error_cb,
				  gpointer                user_data)
{
	SoupReader *r = GINT_TO_POINTER (tag);

	r->headers_done_cb = headers_done_cb;
	r->read_chunk_cb = read_chunk_cb;
	r->read_done_cb = read_done_cb;
	r->error_cb = error_cb;

	r->user_data = user_data;
}

static void
issue_final_callback (SoupReader *r)
{
	/* 
	 * Null terminate 
	 */
	g_byte_array_append (r->recv_buf, "\0", 1);

	if (r->read_done_cb) {
		SoupDataBuffer buf = {
			SOUP_BUFFER_SYSTEM_OWNED,
			r->recv_buf->data,
			r->recv_buf->len - 1
		};

		r->callback_issued = TRUE;

		IGNORE_CANCEL (r);
		(*r->read_done_cb) (&buf, r->user_data);
		UNIGNORE_CANCEL (r);
	}
}

static gboolean
soup_transfer_read_error_cb (GIOChannel* iochannel,
			     GIOCondition condition,
			     SoupReader *r)
{
	gboolean body_started = r->recv_buf->len > r->header_len;

	/*
	 * Closing the connection to signify EOF is valid if content length is
	 * unknown.
	 */
	if (r->encoding == SOUP_TRANSFER_UNKNOWN) {
		issue_final_callback (r);
		goto CANCELLED;
	}

	IGNORE_CANCEL (r);
	if (r->error_cb) (*r->error_cb) (body_started, r->user_data);
	UNIGNORE_CANCEL (r);

 CANCELLED:
	soup_transfer_read_cancel (GPOINTER_TO_INT (r));

	return FALSE;
}

static void
remove_block_at_index (GByteArray *arr, gint offset, gint length)
{
	gchar *data;

	g_return_if_fail (length != 0);
	g_assert (arr->len >= (guint) offset + length);

	data = &arr->data [offset];

	g_memmove (data,
		   data + length,
		   arr->len - offset - length);

	g_byte_array_set_size (arr, arr->len - length);
}

/* 
 * Count number of hex digits, and convert to decimal. Store number of hex
 * digits read in @width.
 */
static gint
decode_hex (const gchar *src, gint *width)
{
	gint new_len = 0, j;

	*width = 0;

	while (isxdigit (*src)) {
		(*width)++;
		src++;
	}
	src -= *width;

	for (j = *width - 1; j + 1; j--) {
		if (isdigit (*src))
			new_len += (*src - 0x30) << (4*j);
		else
			new_len += (tolower (*src) - 0x57) << (4*j);
		src++;
	}

	return new_len;
}

static gboolean
decode_chunk (SoupTransferChunkState *s,
	      GByteArray             *arr,
	      gint                   *datalen) 
{
	gboolean ret = FALSE;

	*datalen = 0;

	while (TRUE) {
		gint new_len = 0;
		gint len = 0;
		gchar *i = &arr->data [s->idx + s->len];

		/*
		 * Not enough data to finish the chunk (and the smallest
		 * possible next chunk header), break 
		 */
		if (s->idx + s->len + 5 > arr->len)
			break;

		/* 
		 * Check for end of chunk header, otherwise break. Avoid
		 * trailing \r\n from previous chunk body if this is not the
		 * opening chunk.  
		 */
		if (s->len) {
			if (soup_substring_index (
					i + 2,
					arr->len - s->idx - s->len - 2,
					"\r\n") <= 0)
				break;
		} else if (soup_substring_index (arr->data,
						 arr->len, 
						 "\r\n") <= 0)
				break;

		/* 
		 * Remove trailing \r\n after previous chunk body 
		 */
		if (s->len)
			remove_block_at_index (arr, s->idx + s->len, 2);

		new_len = decode_hex (i, &len);
		g_assert (new_len >= 0);

		/* 
		 * Previous chunk is now processed, add its length to index and
		 * datalen.
		 */
		s->idx += s->len;
		*datalen += s->len;

		/* 
		 * Update length for next chunk's size 
		 */
		s->len = new_len;
		
	       	/* 
		 * FIXME: Add entity headers we find here to
		 *        req->response_headers. 
		 */
		len += soup_substring_index (&arr->data [s->idx + len],
				             arr->len - s->idx - len,
					     "\r\n");

		/* 
		 * Zero-length chunk closes transfer. Include final \r\n after
                 * empty chunk.
		 */
		if (s->len == 0) {
			len += 2;
			ret = TRUE;
		}

		/* 
		 * Remove hexified length, entity headers, and trailing \r\n 
		 */
		remove_block_at_index (arr, s->idx, len + 2);
	}

	return ret;
}

static void
issue_chunk_callback (SoupReader *r, gchar *data, gint len, gboolean *cancelled)
{
	/* 
	 * Call chunk callback. Pass len worth of data. 
	 */
	if (r->read_chunk_cb && len) {
		SoupTransferDone cont = SOUP_TRANSFER_CONTINUE;
		SoupDataBuffer buf = { 
			SOUP_BUFFER_SYSTEM_OWNED, 
			data,
			len
		};

		r->callback_issued = TRUE;

		IGNORE_CANCEL (r);
		cont = (*r->read_chunk_cb) (&buf, r->user_data);
		UNIGNORE_CANCEL (r);

		if (cont == SOUP_TRANSFER_END)
			*cancelled = TRUE;
		else
			*cancelled = FALSE;
	}
}

static gboolean
read_chunk (SoupReader *r, gboolean *cancelled)
{
	SoupTransferChunkState *s = &r->chunk_state;
	GByteArray *arr = r->recv_buf;
	gboolean ret;
	gint datalen;

	/* 
	 * Update datalen for any data read 
	 */
	ret = decode_chunk (&r->chunk_state, r->recv_buf, &datalen);

	if (!datalen) 
		goto CANCELLED;

	issue_chunk_callback (r, arr->data, s->idx, cancelled);
	if (*cancelled) goto CANCELLED;

	/* 
	 * If overwrite, remove datalen worth of data from start of buffer 
	 */
	if (r->overwrite_chunks) {
		remove_block_at_index (arr, 0, s->idx);

		s->idx = 0;
	}

 CANCELLED:
	return ret;
}

static gboolean
read_content_length (SoupReader *r, gboolean *cancelled)
{
	GByteArray *arr = r->recv_buf;

	if (!arr->len)
		goto CANCELLED;

	issue_chunk_callback (r, arr->data, arr->len, cancelled);
	if (*cancelled) goto CANCELLED;

	/* 
	 * If overwrite, clear 
	 */
	if (r->overwrite_chunks) {
		r->content_length -= r->recv_buf->len;
		g_byte_array_set_size (arr, 0);
	}

 CANCELLED:
	return r->content_length == arr->len;
}

static gboolean
read_unknown (SoupReader *r, gboolean *cancelled)
{
	GByteArray *arr = r->recv_buf;

	if (!arr->len)
		goto CANCELLED;

	issue_chunk_callback (r, arr->data, arr->len, cancelled);
	if (*cancelled) goto CANCELLED;

	/* 
	 * If overwrite, clear 
	 */
	if (r->overwrite_chunks)
		g_byte_array_set_size (arr, 0);

 CANCELLED:
	/* 
	 * Keep reading until we get a zero read or HUP.
	 */
	return FALSE;
}

static gboolean
soup_transfer_read_cb (GIOChannel   *iochannel,
		       GIOCondition  condition,
		       SoupReader   *r)
{
	gchar read_buf [RESPONSE_BLOCK_SIZE];
	gint bytes_read = 0, total_read = 0;
	gboolean read_done = FALSE;
	gboolean cancelled = FALSE;
	GIOError error;

 READ_AGAIN:
	error = g_io_channel_read (iochannel,
				   read_buf,
				   sizeof (read_buf),
				   &bytes_read);

	if (error == G_IO_ERROR_AGAIN) {
		if (total_read) 
			goto PROCESS_READ;
		else return TRUE;
	}

	if (error != G_IO_ERROR_NONE) {
		if (total_read) 
			goto PROCESS_READ;
		else {
			soup_transfer_read_error_cb (iochannel, G_IO_HUP, r);
			return FALSE;
		}
	}

	if (bytes_read) {
		DUMP_READ (read_buf, bytes_read);

		g_byte_array_append (r->recv_buf, read_buf, bytes_read);
		total_read += bytes_read;

		goto READ_AGAIN;
	}

 PROCESS_READ:
	if (r->header_len == 0) {
		gint index;

		index = soup_substring_index (r->recv_buf->data,
					      r->recv_buf->len,
					      "\r\n\r\n");
		if (index < 0) 
			return TRUE;
		else
			index += 4;

		if (r->headers_done_cb) {
			GString str;
			SoupTransferDone ret;

			str.len = index;
			str.str = alloca (index + 1);
			strncpy (str.str, r->recv_buf->data, index);
			str.str [index] = '\0';

			IGNORE_CANCEL (r);
			ret = (*r->headers_done_cb) (&str, 
						     &r->encoding, 
						     &r->content_length, 
						     r->user_data);
			UNIGNORE_CANCEL (r);

			if (ret == SOUP_TRANSFER_END) 
				goto FINISH_READ;
		}

		remove_block_at_index (r->recv_buf, 0, index);
		r->header_len = index;
	}

	if (total_read == 0)
		read_done = TRUE;
	else {
		switch (r->encoding) {
		case SOUP_TRANSFER_CHUNKED:
			read_done = read_chunk (r, &cancelled);
			break;
		case SOUP_TRANSFER_CONTENT_LENGTH:
			read_done = read_content_length (r, &cancelled);
			break;
		case SOUP_TRANSFER_UNKNOWN:
			read_done = read_unknown (r, &cancelled);
			break;
		}
	}

	if (cancelled) 
		goto FINISH_READ;

	if (!read_done) {
		total_read = 0;
		goto READ_AGAIN;
	}

	issue_final_callback (r);

 FINISH_READ:
	soup_transfer_read_cancel (GPOINTER_TO_INT (r));

	return FALSE;
}

guint
soup_transfer_read (GIOChannel            *chan,
		    gboolean               overwrite_chunks,
		    SoupReadHeadersDoneFn  headers_done_cb,
		    SoupReadChunkFn        read_chunk_cb,
		    SoupReadDoneFn         read_done_cb,
		    SoupReadErrorFn        error_cb,
		    gpointer               user_data)
{
	SoupReader *reader;

	reader = g_new0 (SoupReader, 1);
	reader->channel = chan;
	reader->overwrite_chunks = overwrite_chunks;
	reader->headers_done_cb = headers_done_cb;
	reader->read_chunk_cb = read_chunk_cb;
	reader->read_done_cb = read_done_cb;
	reader->error_cb = error_cb;
	reader->user_data = user_data;
	reader->recv_buf = g_byte_array_new ();
	reader->encoding = SOUP_TRANSFER_UNKNOWN;

	reader->read_tag =
		g_io_add_watch (chan,
				G_IO_IN,
				(GIOFunc) soup_transfer_read_cb,
				reader);

	reader->err_tag =
		g_io_add_watch (chan,
				G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				(GIOFunc) soup_transfer_read_error_cb,
				reader);

	return GPOINTER_TO_INT (reader);
}

void
soup_transfer_write_cancel (guint tag)
{
	SoupWriter *w = GINT_TO_POINTER (tag);

	if (w->processing) return;

	g_source_remove (w->write_tag);
	g_source_remove (w->err_tag);

	g_byte_array_free (w->write_buf, TRUE);

	g_free (w);
}

static gboolean
soup_transfer_write_error_cb (GIOChannel* iochannel,
			      GIOCondition condition,
			      SoupWriter *w)
{
	if (w->error_cb) {
		IGNORE_CANCEL (w);
		(*w->error_cb) (w->headers_done, w->user_data);
		UNIGNORE_CANCEL (w);
	}

	soup_transfer_write_cancel (GPOINTER_TO_INT (w));

	return FALSE;
}

static void
write_chunk_sep (GByteArray *arr, gint len, gboolean first_chunk)
{
	gchar *hex;
	gchar *end = "\r\n0\r\n";

	if (len) {
		hex = g_strdup_printf (first_chunk ? "%x\r\n" : "\r\n%x\r\n", 
				       len);
		g_byte_array_append (arr, hex, strlen (hex));
		g_free (hex);
	} else
		g_byte_array_append (arr, end, strlen (end));
} 

static void
write_chunk (SoupWriter *w, const SoupDataBuffer *buf)
{
	if (w->encoding == SOUP_TRANSFER_CHUNKED) {
		write_chunk_sep (w->write_buf, buf->length, w->chunk_cnt);
		w->chunk_cnt++;
	}

	g_byte_array_append (w->write_buf, buf->body, buf->length);
}

#ifdef SIGPIPE
#  define IGNORE_PIPE(pipe_handler) pipe_handler = signal (SIGPIPE, SIG_IGN)
#  define RESTORE_PIPE(pipe_handler) signal (SIGPIPE, pipe_handler)
#else
#  define IGNORE_PIPE(x)
#  define RESTORE_PIPE(x)
#endif

static gboolean
soup_transfer_write_cb (GIOChannel* iochannel,
			GIOCondition condition,
			SoupWriter *w)
{
	GIOError error;
	gpointer pipe_handler;
	gint bytes_written = 0;

	IGNORE_PIPE (pipe_handler);
	errno = 0;

 WRITE_AGAIN:
	while (w->write_buf->len) {
		error = g_io_channel_write (iochannel,
					    w->write_buf->data,
					    w->write_buf->len,
					    &bytes_written);

		if (error == G_IO_ERROR_AGAIN) 
			goto TRY_AGAIN;

		if (errno != 0 || error != G_IO_ERROR_NONE) {
			soup_transfer_write_error_cb (iochannel, G_IO_HUP, w);
			goto DONE_WRITING;
		}

		if (!bytes_written) 
			goto TRY_AGAIN;

		if (!w->headers_done && bytes_written >= w->header_len) {
			if (w->headers_done_cb) {
				IGNORE_CANCEL (w);
				(*w->headers_done_cb) (w->user_data);
				UNIGNORE_CANCEL (w);
			}
			w->headers_done = TRUE;
		}

		DUMP_WRITE (w->write_buf->data, bytes_written);

		remove_block_at_index (w->write_buf, 0, bytes_written);
	}

	if (w->write_chunk_cb) {
		SoupTransferStatus ret = SOUP_TRANSFER_END;
		SoupDataBuffer *buf = NULL;

		IGNORE_CANCEL (w);
		ret = (*w->write_chunk_cb) (&buf, w->user_data);
		UNIGNORE_CANCEL (w);

		if (buf && buf->length) {
			write_chunk (w, buf);
			goto WRITE_AGAIN;
		}

		if (ret == SOUP_TRANSFER_CONTINUE)
			goto TRY_AGAIN;
		else if (w->encoding == SOUP_TRANSFER_CHUNKED)
			write_chunk_sep (w->write_buf, 0, w->chunk_cnt);
	}

	if (w->write_done_cb) {
		IGNORE_CANCEL (w);
		(*w->write_done_cb) (w->user_data);
		UNIGNORE_CANCEL (w);
	}

	soup_transfer_write_cancel (GPOINTER_TO_INT (w));

 DONE_WRITING:
	RESTORE_PIPE (pipe_handler);
	return FALSE;

 TRY_AGAIN:
	RESTORE_PIPE (pipe_handler);
	return TRUE;
}

guint
soup_transfer_write (GIOChannel             *chan,
		     const GString          *header,
		     const SoupDataBuffer   *src,
		     SoupTransferEncoding    encoding,
		     SoupWriteHeadersDoneFn  headers_done_cb,
		     SoupWriteChunkFn        write_chunk_cb,
		     SoupWriteDoneFn         write_done_cb,
		     SoupWriteErrorFn        error_cb,
		     gpointer                user_data)
{
	SoupWriter *writer;

	writer = g_new0 (SoupWriter, 1);
	writer->channel = chan;
	writer->encoding = encoding;
	writer->headers_done_cb = headers_done_cb;
	writer->write_chunk_cb = write_chunk_cb;
	writer->write_done_cb = write_done_cb;
	writer->error_cb = error_cb;
	writer->user_data = user_data;
	writer->write_buf = g_byte_array_new ();

	if (header && header->len) {
		g_byte_array_append (writer->write_buf, 
				     header->str, 
				     header->len);
		writer->header_len = header->len;
	}

	if (src && src->length)
		write_chunk (writer, src);

	if (write_chunk_cb) {
		SoupTransferStatus ret = SOUP_TRANSFER_END;
		SoupDataBuffer *buf = NULL;

		IGNORE_CANCEL (writer);
		ret = (*write_chunk_cb) (&buf, user_data);
		UNIGNORE_CANCEL (writer);

		if (buf && buf->length)
			write_chunk (writer, buf);

		if (ret == SOUP_TRANSFER_END) {
			writer->write_chunk_cb = NULL;

			if (writer->encoding == SOUP_TRANSFER_CHUNKED)
				write_chunk_sep (writer->write_buf, 
						 0, 
						 writer->chunk_cnt);
		}
	}

	writer->write_tag =
		g_io_add_watch (chan,
				G_IO_OUT,
				(GIOFunc) soup_transfer_write_cb,
				writer);

	writer->err_tag =
		g_io_add_watch (chan,
				G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				(GIOFunc) soup_transfer_write_error_cb,
				writer);

	return GPOINTER_TO_INT (writer);
}
