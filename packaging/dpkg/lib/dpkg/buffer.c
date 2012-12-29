/*
 * libdpkg - Debian packaging suite library routines
 * buffer.c - buffer I/O handling routines
 *
 * Copyright © 1999, 2000 Wichert Akkerman <wakkerma@debian.org>
 * Copyright © 2000-2003 Adam Heath <doogie@debian.org>
 * Copyright © 2008-2012 Guillem Jover <guillem@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <compat.h>

#include <sys/types.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <dpkg/i18n.h>
#include <dpkg/dpkg.h>
#include <dpkg/varbuf.h>
#include <dpkg/md5.h>
#include <dpkg/fdio.h>
#include <dpkg/buffer.h>

struct buffer_md5_ctx {
	struct MD5Context ctx;
	char *hash;
};

static void
buffer_md5_init(struct buffer_data *data)
{
	struct buffer_md5_ctx *ctx;

	ctx = m_malloc(sizeof(*ctx));
	ctx->hash = data->arg.ptr;
	data->arg.ptr = ctx;
	MD5Init(&ctx->ctx);
}

static off_t
buffer_filter_init(struct buffer_data *data)
{
	switch (data->type) {
	case BUFFER_FILTER_NULL:
		break;
	case BUFFER_FILTER_MD5:
		buffer_md5_init(data);
		break;
	}
	return 0;
}

static off_t
buffer_filter_update(struct buffer_data *filter, const void *buf, off_t length)
{
	off_t ret = length;

	switch (filter->type) {
	case BUFFER_FILTER_NULL:
		break;
	case BUFFER_FILTER_MD5:
		MD5Update(&(((struct buffer_md5_ctx *)filter->arg.ptr)->ctx),
		          buf, length);
		break;
	default:
		internerr("unknown data type %i", filter->type);
	}

	return ret;
}

static void
buffer_md5_done(struct buffer_data *data)
{
	struct buffer_md5_ctx *ctx;
	unsigned char digest[16], *p = digest;
	char *hash;
	int i;

	ctx = (struct buffer_md5_ctx *)data->arg.ptr;
	hash = ctx->hash;
	MD5Final(digest, &ctx->ctx);
	for (i = 0; i < 16; ++i) {
		sprintf(hash, "%02x", *p++);
		hash += 2;
	}
	*hash = '\0';
	free(ctx);
}

static off_t
buffer_filter_done(struct buffer_data *data)
{
	switch (data->type) {
	case BUFFER_FILTER_NULL:
		break;
	case BUFFER_FILTER_MD5:
		buffer_md5_done(data);
		break;
	}
	return 0;
}

static off_t
buffer_write(struct buffer_data *data, const void *buf, off_t length,
             struct dpkg_error *err)
{
	off_t ret = length;

	switch (data->type) {
	case BUFFER_WRITE_VBUF:
		varbuf_add_buf((struct varbuf *)data->arg.ptr, buf, length);
		break;
	case BUFFER_WRITE_FD:
		ret = fd_write(data->arg.i, buf, length);
		if (ret < 0)
			dpkg_put_errno(err, _("failed to write"));
		break;
	case BUFFER_WRITE_NULL:
		break;
	default:
		internerr("unknown data type %i", data->type);
	}

	return ret;
}

static off_t
buffer_read(struct buffer_data *data, void *buf, off_t length,
            struct dpkg_error *err)
{
	off_t ret;

	switch (data->type) {
	case BUFFER_READ_FD:
		ret = fd_read(data->arg.i, buf, length);
		if (ret < 0)
			dpkg_put_errno(err, _("failed to read"));
		break;
	default:
		internerr("unknown data type %i", data->type);
	}

	return ret;
}

off_t
buffer_filter(const void *input, void *output, int type, off_t limit)
{
	struct buffer_data data = { .arg.ptr = output, .type = type };
	off_t ret;

	buffer_filter_init(&data);
	ret = buffer_filter_update(&data, input, limit);
	buffer_filter_done(&data);

	return ret;
}

static off_t
buffer_copy(struct buffer_data *read_data,
            struct buffer_data *filter,
            struct buffer_data *write_data,
            off_t limit, struct dpkg_error *err)
{
	char *buf;
	int bufsize = 32768;
	off_t bytesread = 0, byteswritten = 0;
	off_t totalread = 0, totalwritten = 0;

	if ((limit != -1) && (limit < bufsize))
		bufsize = limit;
	if (bufsize == 0)
		buf = NULL;
	else
		buf = m_malloc(bufsize);

	buffer_filter_init(filter);

	while (bufsize > 0) {
		bytesread = buffer_read(read_data, buf, bufsize, err);
		if (bytesread < 0)
			return -1;
		if (bytesread == 0)
			break;

		totalread += bytesread;

		if (limit != -1) {
			limit -= bytesread;
			if (limit < bufsize)
				bufsize = limit;
		}

		buffer_filter_update(filter, buf, bytesread);

		byteswritten = buffer_write(write_data, buf, bytesread, err);
		if (byteswritten < 0)
			return -1;
		if (byteswritten == 0)
			break;

		totalwritten += byteswritten;
	}

	if (limit > 0)
		return dpkg_put_error(err, _("unexpected end of file or stream"));

	buffer_filter_done(filter);

	free(buf);

	return totalread;
}

off_t
buffer_copy_IntInt(int Iin, int Tin,
                   void *Pfilter, int Tfilter,
                   int Iout, int Tout,
                   off_t limit, struct dpkg_error *err)
{
	struct buffer_data read_data = { .type = Tin, .arg.i = Iin };
	struct buffer_data filter = { .type = Tfilter, .arg.ptr = Pfilter };
	struct buffer_data write_data = { .type = Tout, .arg.i = Iout };

	return buffer_copy(&read_data, &filter, &write_data, limit, err);
}

off_t
buffer_copy_IntPtr(int Iin, int Tin,
                   void *Pfilter, int Tfilter,
                   void *Pout, int Tout,
                   off_t limit, struct dpkg_error *err)
{
	struct buffer_data read_data = { .type = Tin, .arg.i = Iin };
	struct buffer_data filter = { .type = Tfilter, .arg.ptr = Pfilter };
	struct buffer_data write_data = { .type = Tout, .arg.ptr = Pout };

	return buffer_copy(&read_data, &filter, &write_data, limit, err);
}

static off_t
buffer_skip(struct buffer_data *input, off_t limit, struct dpkg_error *err)
{
	struct buffer_data output;
	struct buffer_data filter;

	switch (input->type) {
	case BUFFER_READ_FD:
		if (lseek(input->arg.i, limit, SEEK_CUR) != -1)
			return limit;
		if (errno != ESPIPE)
			return dpkg_put_errno(err, _("failed to seek"));
		break;
	default:
		internerr("unknown data type %i", input->type);
	}

	output.type = BUFFER_WRITE_NULL;
	output.arg.ptr = NULL;
	filter.type = BUFFER_FILTER_NULL;
	filter.arg.ptr = NULL;

	return buffer_copy(input, &filter, &output, limit, err);
}

off_t
buffer_skip_Int(int I, int T, off_t limit, struct dpkg_error *err)
{
	struct buffer_data input = { .type = T, .arg.i = I };

	return buffer_skip(&input, limit, err);
}
