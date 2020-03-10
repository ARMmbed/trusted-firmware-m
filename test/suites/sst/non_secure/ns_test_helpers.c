/*
 * Copyright (c) 2018-2019, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ns_test_helpers.h"

#include "tfm_nspm_api.h"
#include "cmsis_os2.h"

#define SST_TEST_TASK_STACK_SIZE (768)

struct test_task_t {
    test_func_t *func;
    struct test_result_t *ret;
};

static osSemaphoreId_t test_semaphore;

/**
 * \brief Executes the supplied test task and then releases the test semaphore.
 *
 * \param[in,out] arg  Pointer to the test task. Must be a pointer to a
 *                     struct test_task_t
 */
static void test_task_runner(void *arg)
{
    struct test_task_t *test = arg;

#ifdef TFM_NS_CLIENT_IDENTIFICATION
    tfm_nspm_register_client_id();
#endif /* TFM_NS_CLIENT_IDENTIFICATION */

    /* Call the test function */
    test->func(test->ret);

    /* Release the semaphore to unblock the parent thread */
    osSemaphoreRelease(test_semaphore);

    /* Signal to the RTOS that the thread is finished */
    osThreadExit();
}

void tfm_sst_run_test(const char *thread_name, struct test_result_t *ret,
                      test_func_t *test_func)
{
    osThreadId_t current_thread_handle;
    osPriority_t current_thread_priority;
    osThreadId_t thread;
    struct test_task_t test_task = { .func = test_func, .ret = ret };
    osSemaphoreAttr_t sema_attrib = {
        .name = "sst_tests_sema",
    };
    osThreadAttr_t task_attribs = {
        .tz_module = 1,
        .name = thread_name,
        .stack_size = SST_TEST_TASK_STACK_SIZE,
    };

    /* Create a binary semaphore with initial count of 0 tokens available */
    test_semaphore = osSemaphoreNew(1, 0, &sema_attrib);
    if (!test_semaphore) {
        TEST_FAIL("Semaphore creation failed");
        return;
    }

    current_thread_handle = osThreadGetId();
    if (!current_thread_handle) {
        osSemaphoreDelete(test_semaphore);
        TEST_FAIL("Failed to get current thread ID");
        return;
    }

    current_thread_priority = osThreadGetPriority(current_thread_handle);
    if (current_thread_priority == osPriorityError) {
        osSemaphoreDelete(test_semaphore);
        TEST_FAIL("Failed to get current thread priority");
        return;
    }
    task_attribs.priority = current_thread_priority;

    /* Start test thread */
    thread = osThreadNew(test_task_runner, &test_task, &task_attribs);
    if (!thread) {
        osSemaphoreDelete(test_semaphore);
        TEST_FAIL("Failed to create test thread");
        return;
    }

    /* Signal semaphore, wait indefinitely until unblocked by child thread */
    osSemaphoreAcquire(test_semaphore, osWaitForever);

    /* At this point, it means the binary semaphore has been released by the
     * test and re-acquired by this thread, so just finally release it and
     * delete it
     */
    osSemaphoreRelease(test_semaphore);

    osSemaphoreDelete(test_semaphore);
}
