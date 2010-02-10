
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

/** repeats write after EINTR */
ssize_t li_net_write(int fd, void *buf, ssize_t nbyte) {
	ssize_t r;
	while (-1 == (r = write(fd, buf, nbyte))) {
		switch (errno) {
		case EINTR:
			/* Try again */
			break;
		default:
			/* report error */
			return r;
		}
	}
	/* return bytes written */
	return r;
}

/** repeats read after EINTR */
ssize_t li_net_read(int fd, void *buf, ssize_t nbyte) {
	ssize_t r;
	while (-1 == (r = read(fd, buf, nbyte))) {
		switch (errno) {
		case EINTR:
			/* Try again */
			break;
		default:
			/* report error */
			return r;
		}
	}
	/* return bytes read */
	return r;
}

liNetworkStatus li_network_write(liVRequest *vr, int fd, liChunkQueue *cq, goffset write_max) {
	liNetworkStatus res;
#ifdef TCP_CORK
	int corked = 0;
#endif
	goffset write_bytes, wrote;

#ifdef TCP_CORK
	/* Linux: put a cork into the socket as we want to combine the write() calls
	 * but only if we really have multiple chunks
	 */
	if (cq->queue.length > 1) {
		corked = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	write_bytes = write_max;
	/* TODO: add setup-option to select the backend */
#ifdef USE_SENDFILE
	res = li_network_write_sendfile(vr, fd, cq, &write_bytes);
#else
	res = li_network_write_writev(vr, fd, cq, &write_bytes);
#endif
	wrote = write_max - write_bytes;
	if (wrote > 0 && res == LI_NETWORK_STATUS_WAIT_FOR_EVENT) res = LI_NETWORK_STATUS_SUCCESS;

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	return res;
}

liNetworkStatus li_network_read(liVRequest *vr, int fd, liChunkQueue *cq, liBuffer **buffer) {
	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;

	if (cq->limit && cq->limit->limit > 0) {
		if (max_read > cq->limit->limit - cq->limit->current) {
			max_read = cq->limit->limit - cq->limit->current;
			if (max_read <= 0) {
				max_read = 0; /* we still have to read something */
				VR_ERROR(vr, "%s", "li_network_read: fd should be disabled as chunkqueue is already full");
			}
		}
	}

	do {
		liBuffer *buf = NULL;
		gboolean cq_buf_append;

		buf = li_chunkqueue_get_last_buffer(cq, 1024);
		cq_buf_append = (buf != NULL);

		if (NULL != buffer) {
			if (buf != NULL) {
				/* use last buffer as *buffer; they should be the same anyway */
				if (G_UNLIKELY(buf != *buffer)) {
					li_buffer_acquire(buf);
					li_buffer_release(*buffer);
					*buffer = buf;
				}
			} else {
				buf = *buffer;
				if (buf != NULL) {
					/* if *buffer is the only reference, we can reset the buffer */
					if (g_atomic_int_get(&buf->refcount) == 1) {
						buf->used = 0;
					}

					if (buf->alloc_size - buf->used < 1024) {
						/* release *buffer */
						li_buffer_release(buf);
						*buffer = buf = NULL;
					}
				}
				if (buf == NULL) {
					*buffer = buf = li_buffer_new(blocksize);
				}
			}
			assert(*buffer == buf);
		} else {
			if (buf == NULL) {
				buf = li_buffer_new(blocksize);
			}
		}

		if (-1 == (r = li_net_read(fd, buf->addr + buf->used, buf->alloc_size - buf->used))) {
			if (buffer == NULL && !cq_buf_append) li_buffer_release(buf);
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return len ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
			case ETIMEDOUT:
				return LI_NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				VR_ERROR(vr, "oops, read from fd=%d failed: %s", fd, g_strerror(errno) );
				return LI_NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			if (buffer == NULL && !cq_buf_append) li_buffer_release(buf);
			return len ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_CONNECTION_CLOSE;
		}
		if (cq_buf_append) {
			li_chunkqueue_update_last_buffer_size(cq, r);
		} else {
			gsize offset;

			if (buffer != NULL) li_buffer_acquire(buf);

			offset = buf->used;
			buf->used += r;
			li_chunkqueue_append_buffer2(cq, buf, offset, r);
		}
		if (NULL != buffer) {
			if (buf->alloc_size - buf->used < 1024) {
				/* release *buffer */
				li_buffer_release(buf);
				*buffer = buf = NULL;
			}
		}
		len += r;
	} while (r == blocksize && len < max_read);

	return LI_NETWORK_STATUS_SUCCESS;
}
