/* SPDX-License-Identifier: MIT */
/*
 * Description: test waitid functionality
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "liburing.h"
#include "helpers.h"

static bool no_waitid;

static void child(long msleep_time)
{
	usleep(msleep_time * 1000UL);
	exit(0);
}

/*
 * Test linked timeout with child not exiting in time
 */
static int test_noexit(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	siginfo_t si;
	pid_t pid;
	int ret, i;

	pid = fork();
	if (!pid) {
		child(200);
		exit(0);
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_waitid(sqe, P_PID, pid, &si, WEXITED);
	sqe->flags |= IOSQE_IO_LINK;
	sqe->user_data = 1;

	ts.tv_sec = 0;
	ts.tv_nsec = 100 * 1000 * 1000ULL;
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_link_timeout(sqe, &ts, 0);
	sqe->user_data = 2;

	io_uring_submit(ring);

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "cqe wait: %d\n", ret);
			return T_EXIT_FAIL;
		}
		if (cqe->user_data == 2 && cqe->res != 1) {
			fprintf(stderr, "timeout res: %d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		if (cqe->user_data == 1 && cqe->res != -ECANCELED) {
			fprintf(stderr, "waitid res: %d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return T_EXIT_PASS;
}

/*
 * Test one child exiting, but not the one we were looking for
 */
static int test_double(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	siginfo_t si;
	pid_t p1, p2;
	int ret;

	/* p1 will exit shortly */
	p1 = fork();
	if (!p1) {
		child(100);
		exit(0);
	}

	/* p2 will linger */
	p2 = fork();
	if (!p2) {
		child(200);
		exit(0);
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_waitid(sqe, P_PID, p2, &si, WEXITED);

	io_uring_submit(ring);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "cqe wait: %d\n", ret);
		return T_EXIT_FAIL;
	}

	if (cqe->res < 0) {
		fprintf(stderr, "cqe res: %d\n", cqe->res);
		return T_EXIT_FAIL;
	}
	if (si.si_pid != p2) {
		fprintf(stderr, "expected pid %d, got %d\n", p2, si.si_pid);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(ring, cqe);
	return T_EXIT_PASS;
}

/*
 * Test reaping of an already exited task
 */
static int test_ready(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	siginfo_t si;
	pid_t pid;
	int ret;

	pid = fork();
	if (!pid) {
		child(0);
		exit(0);
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_waitid(sqe, P_PID, pid, &si, WEXITED);

	io_uring_submit(ring);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "cqe wait: %d\n", ret);
		return T_EXIT_FAIL;
	}

	if (cqe->res < 0) {
		fprintf(stderr, "cqe res: %d\n", cqe->res);
		return T_EXIT_FAIL;
	}
	if (si.si_pid != pid) {
		fprintf(stderr, "expected pid %d, got %d\n", pid, si.si_pid);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(ring, cqe);
	return T_EXIT_PASS;
}

/*
 * Test cancelation of pending waitid
 */
static int test_cancel(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret, i;
	pid_t pid;

	pid = fork();
	if (!pid) {
		child(1000);
		exit(0);
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_waitid(sqe, P_PID, pid, NULL, WEXITED);
	sqe->user_data = 1;

	io_uring_submit(ring);

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_cancel64(sqe, 1, 0);
	sqe->user_data = 2;

	io_uring_submit(ring);

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "cqe wait: %d\n", ret);
			return T_EXIT_FAIL;
		}
		if (cqe->user_data == 1 && cqe->res != -ECANCELED) {
			fprintf(stderr, "cqe res: %d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		if (cqe->user_data == 2 && cqe->res != 1) {
			fprintf(stderr, "cqe res: %d\n", cqe->res);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return T_EXIT_PASS;
}

/*
 * Test basic reap of child exit
 */
static int test(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	siginfo_t si;
	pid_t pid;
	int ret;

	pid = fork();
	if (!pid) {
		child(100);
		exit(0);
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_waitid(sqe, P_PID, pid, &si, WEXITED);

	io_uring_submit(ring);

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "cqe wait: %d\n", ret);
		return T_EXIT_FAIL;
	}

	/* no waitid support */
	if (cqe->res == -EINVAL) {
		no_waitid = true;
		return T_EXIT_SKIP;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "cqe res: %d\n", cqe->res);
		return T_EXIT_FAIL;
	}
	if (si.si_pid != pid) {
		fprintf(stderr, "expected pid %d, got %d\n", pid, si.si_pid);
		return T_EXIT_FAIL;
	}

	io_uring_cqe_seen(ring, cqe);
	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	io_uring_queue_init(8, &ring, 0);

	ret = test(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test failed\n");
		return T_EXIT_FAIL;
	}
	if (no_waitid)
		return T_EXIT_SKIP;

	ret = test_noexit(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_noexit failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_noexit(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_noexit failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_double(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_double failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_ready(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_ready failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_cancel(&ring);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_cancel failed\n");
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}
