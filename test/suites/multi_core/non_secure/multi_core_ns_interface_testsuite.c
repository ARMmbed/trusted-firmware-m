/*
 * Copyright (c) 2019-2020, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include "psa/client.h"
#include "psa/internal_trusted_storage.h"
#include "psa_manifest/sid.h"
#include "test/framework/test_framework_helpers.h"
#include "tfm_ns_mailbox.h"
#include "cmsis_os2.h"

/* Max number of child threads for multiple outstanding PSA client call test */
#define NR_MULTI_CALL_CHILD                   (NUM_MAILBOX_QUEUE_SLOT * 2)

/* The event flag to sync up between parent thread and child threads */
#define TEST_CHILD_EVENT_FLAG(x)              (uint32_t)(0x1UL << (x))

/* Max number of test rounds */
#define MAX_NR_LIGHT_TEST_ROUND               0x200
#define MAX_NR_HEAVY_TEST_ROUND               0x20

/* Default stack size for child thread */
#define MULTI_CALL_LIGHT_TEST_STACK_SIZE      0x200
#define MULTI_CALL_HEAVY_TEST_STACK_SIZE      0x300

/* Test UID copied from ITS test cases */
#define TEST_UID_1                            2U
/* ITS data for multiple PSA client call heavy tests */
#define ITS_DATA                               "ITSDataForMultiCore"
#define ITS_DATA_LEN                           sizeof(ITS_DATA)

/* Structure passed to test threads */
struct test_params {
    void *parent_handle;            /* The thread handle of parent thread */
    uint32_t child_idx;             /* The index of current child thread */
    uint32_t nr_rounds;             /* The number of test rounds */
    void *mutex_handle;             /* Mutex to protect is_complete flag */
    enum test_status_t ret;         /* The test result */
    bool is_complete;               /* Whether current test thread completes */
    bool is_parent;                 /* Whether executed in parent thread */
};

/* Multiple outstanding PSA client call test secure service SID and version */
struct multi_call_service_info {
    uint32_t sid;
    uint32_t version;
};

/*
 * If not enough secure services are defined for multiple outstanding PSA
 * client call test, the definitions below will trigger compiling error.
 */
const static struct multi_call_service_info multi_call_service_list[] = {
    {MULTI_CORE_MULTI_CLIENT_CALL_TEST_0_SID,
     MULTI_CORE_MULTI_CLIENT_CALL_TEST_0_VERSION},
    {MULTI_CORE_MULTI_CLIENT_CALL_TEST_1_SID,
     MULTI_CORE_MULTI_CLIENT_CALL_TEST_1_VERSION}
};

/* List of tests */
static void multi_client_call_light_test(struct test_result_t *ret);
static void multi_client_call_heavy_test(struct test_result_t *ret);

static struct test_t multi_core_tests[] = {
    {&multi_client_call_light_test,
     "MULTI_CLIENT_CALL_LIGHT_TEST",
     "Multiple outstanding NS PSA client calls lightweight test", {TEST_PASSED}},
    {&multi_client_call_heavy_test,
     "MULTI_CLIENT_CALL_HEAVY_TEST",
     "Multiple outstanding NS PSA client calls heavyweight test", {TEST_PASSED}},
};

void register_testsuite_multi_core_ns_interface(
                                              struct test_suite_t *p_test_suite)
{
    uint32_t list_size;

    list_size = (sizeof(multi_core_tests) / sizeof(multi_core_tests[0]));

    set_testsuite("TF-M test cases for multi-core topology",
                  multi_core_tests, list_size, p_test_suite);
}

static void wait_child_thread_completion(struct test_params *params_array,
                                         uint8_t child_idx)
{
    bool is_complete;
    uint8_t i;
    osMutexId_t mutex = params_array[0].mutex_handle;

    for (i = 0; i < child_idx; i++) {
        while (1) {
            osMutexAcquire(mutex, osWaitForever);
            is_complete = params_array[i].is_complete;
            osMutexRelease(mutex);

            if (is_complete) {
                break;
            }
        }
    }
}

static void multi_client_call_test(struct test_result_t *ret,
                                   osThreadFunc_t test_runner,
                                   int32_t stack_size,
                                   int32_t nr_rounds)
{
    uint8_t i, nr_child;
    osThreadId_t current_thread_handle;
    osPriority_t current_thread_priority;
    osMutexId_t mutex_handle;
    osThreadId_t child_ids[NR_MULTI_CALL_CHILD];
    struct ns_mailbox_stats_res_t stats_res;
    struct test_params parent_params, params[NR_MULTI_CALL_CHILD];
    const osMutexAttr_t attr = {
        .name = NULL,
        .attr_bits = osMutexPrioInherit, /* Priority inheritance is recommended
                                          * to enable if it is supported.
                                          * For recursive mutex and the ability
                                          * of auto release when owner being
                                          * terminated is not required.
                                          */
        .cb_mem = NULL,
        .cb_size = 0U
    };
    osThreadAttr_t task_attribs = {
        .tz_module = 1,
        .stack_size = stack_size,
    };

    tfm_ns_mailbox_tx_stats_init();

    current_thread_handle = osThreadGetId();
    if (!current_thread_handle) {
        TEST_FAIL("Failed to get current thread ID\r\n");
        return;
    }

    current_thread_priority = osThreadGetPriority(current_thread_handle);
    if (current_thread_priority == osPriorityError) {
        TEST_FAIL("Failed to get current thread priority\r\n");
        return;
    }
    task_attribs.priority = current_thread_priority;

    /*
     * Create a mutex to protect the synchronization between child test thread
     * about the completion status.
     * The best way is to use osThreadFlagsWait/Set(). However, due to the
     * implementation of the wait event functions in some RTOS, if the child
     * test threads already exit before the main thread starts to wait for
     * event (main thread itself has to perform test too), the main thread
     * cannot receive the event flags.
     * As a result, use a flag and a mutex to make sure the main thread can
     * capture the completion event of child threads.
     */
    mutex_handle = osMutexNew(&attr);
    if (!mutex_handle) {
        TEST_FAIL("Failed to create a mutex\r\n");
        return;
    }

    /* Create test threads one by one */
    for (i = 0; i < NR_MULTI_CALL_CHILD; i++) {
        params[i].parent_handle = current_thread_handle;
        params[i].child_idx = i;
        params[i].nr_rounds = nr_rounds;
        params[i].mutex_handle = mutex_handle;
        params[i].is_complete = false;
        params[i].is_parent = false;

        child_ids[i] = osThreadNew(test_runner, &params[i], &task_attribs);
        if (!child_ids[i]) {
            break;
        }
    }

    nr_child = i;
    TEST_LOG("Totally %d threads for test start\r\n", nr_child + 1);
    TEST_LOG("Each thread run 0x%x rounds tests\r\n", nr_rounds);

    /*
     * Activate test threads one by one.
     * Try to make test threads to run together.
     */
    for (i = 0; i < nr_child; i++) {
        osThreadFlagsSet(child_ids[i], TEST_CHILD_EVENT_FLAG(i));
    }

    /* Use current thread to execute a test instance */
    parent_params.child_idx = nr_child;
    parent_params.nr_rounds = nr_rounds;
    parent_params.is_parent = true;
    test_runner(&parent_params);

    /* Wait for all the test threads completes */
    wait_child_thread_completion(params, nr_child);

    osMutexDelete(mutex_handle);

    if (parent_params.ret != TEST_PASSED) {
        ret->val = TEST_FAILED;
        return;
    }

    /* Check the test result of each child thread */
    for (i = 0; i < nr_child; i++) {
        if (params[i].ret != TEST_PASSED) {
            ret->val = TEST_FAILED;
            return;
        }
    }

    tfm_ns_mailbox_stats_avg_slot(&stats_res);
    TEST_LOG("Totally %d NS mailbox queue slots\r\n", NUM_MAILBOX_QUEUE_SLOT);
    TEST_LOG("%d.%d NS mailbox queue slots are occupied each time in average.\r\n",
             stats_res.avg_nr_slots, stats_res.avg_nr_slots_tenths);

    ret->val = TEST_PASSED;
}

static inline enum test_status_t multi_client_call_light_loop(uint8_t child_idx,
                                                             uint32_t nr_rounds)
{
    psa_handle_t handle;
    psa_status_t status;
    uint32_t i, nr_calls, nr_service, sid, version;
    struct psa_outvec outvec = {&nr_calls, sizeof(nr_calls)};

    nr_service = sizeof(multi_call_service_list) /
                 sizeof(multi_call_service_list[0]);
    /* Determine the secure service ID and version */
    sid = multi_call_service_list[child_idx % nr_service].sid;
    version = multi_call_service_list[child_idx % nr_service].version;

    for (i = 0; i < nr_rounds; i++) {
        handle = psa_connect(sid, version);
        if (handle <= 0) {
            TEST_LOG("Fail to connect test service!\r\n");
            return TEST_FAILED;
        }

        status = psa_call(handle, PSA_IPC_CALL, NULL, 0, &outvec, 1);
        if (status < 0) {
            TEST_LOG("Fail to call test service\r\n");
            return TEST_FAILED;
        }

        psa_close(handle);
    }

    return TEST_PASSED;
}

static void multi_client_call_light_runner(void *argument)
{
    struct test_params *params = (struct test_params *)argument;

    if (!params->is_parent) {
        /* Wait for the signal to kick-off the test */
        osThreadFlagsWait(TEST_CHILD_EVENT_FLAG(params->child_idx),
                          osFlagsWaitAll,
                          osWaitForever);
    }

    params->ret = multi_client_call_light_loop(params->child_idx,
                                               params->nr_rounds);

    if (!params->is_parent) {
        /* Mark this child thread has completed */
        osMutexAcquire(params->mutex_handle, osWaitForever);
        params->is_complete = true;
        osMutexRelease(params->mutex_handle);
    }
}

/**
 * \brief Lightweight test case to verify multiple outstanding PSA client calls
 *        feature.
 */
static void multi_client_call_light_test(struct test_result_t *ret)
{
    multi_client_call_test(ret, multi_client_call_light_runner,
                           MULTI_CALL_LIGHT_TEST_STACK_SIZE,
                           MAX_NR_LIGHT_TEST_ROUND);
}

static inline enum test_status_t multi_client_call_heavy_loop(
                                                    const psa_storage_uid_t uid,
                                                    uint32_t rounds)
{
    uint32_t i;
    psa_status_t status;
    size_t rd_data_len;
    char rd_data[ITS_DATA_LEN];
    const psa_storage_create_flags_t flags = PSA_STORAGE_FLAG_NONE;

    for (i = 0; i < rounds; i++) {
        /* Set a data in the asset */
        status = psa_its_set(uid, ITS_DATA_LEN, ITS_DATA, flags);
        if (status != PSA_SUCCESS) {
            TEST_LOG("Fail to write ITS asset\r\n");
            return TEST_FAILED;
        }

        /* Get data from the asset */
        status = psa_its_get(uid, 0, ITS_DATA_LEN, rd_data, &rd_data_len);
        if (status != PSA_SUCCESS) {
            TEST_LOG("Fail to read ITS asset\r\n");
            return TEST_FAILED;
        }

        /* Remove the asset to clean up storage */
        status = psa_its_remove(uid);
        if (status != PSA_SUCCESS) {
            TEST_LOG("Fail to remove ITS asset\r\n");
            return TEST_FAILED;
        }
    }

    return TEST_PASSED;
}

static void multi_client_call_heavy_runner(void *argument)
{
    struct test_params *params = (struct test_params *)argument;
    const psa_storage_uid_t uid = TEST_UID_1 + params->child_idx;

    if (!params->is_parent) {
        /* Wait for the signal to kick-off the test */
        osThreadFlagsWait(TEST_CHILD_EVENT_FLAG(params->child_idx),
                          osFlagsWaitAll,
                          osWaitForever);
    }

    params->ret = multi_client_call_heavy_loop(uid, params->nr_rounds);

    if (!params->is_parent) {
        /* Mark this child thread has completed */
        osMutexAcquire(params->mutex_handle, osWaitForever);
        params->is_complete = true;
        osMutexRelease(params->mutex_handle);
    }
}

/**
 * \brief NS interface to verify multiple outstanding PSA client calls feature
 *        by calling heavyweight secure services.
 */
static void multi_client_call_heavy_test(struct test_result_t *ret)
{
    multi_client_call_test(ret, multi_client_call_heavy_runner,
                           MULTI_CALL_HEAVY_TEST_STACK_SIZE,
                           MAX_NR_HEAVY_TEST_ROUND);
}
