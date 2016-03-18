/*
 * Copyright (c) 2016, Technische Universität Dresden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions
 *    and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
 *    and the following disclaimer in the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <math.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <x86_energy.h>

#define ENERGY 0
#define POWER  1
#define SYSENERGY 2
#define SYSPOWER 3

#define GET_OWN_VALUES 1
#define USE_OLD_VALUES 2

#if !defined(BACKEND_SCOREP) && !defined(BACKEND_VTRACE)
#define BACKEND_VTRACE
#endif

#if defined(BACKEND_SCOREP) && defined(BACKEND_VTRACE)
#error "Cannot compile for both VT and Score-P at the same time!\n"
#endif

#ifdef BACKEND_VTRACE
#define ENV_PREFIX "VT_"
#define SET_METRIC_INFO(__minfo) \
            __minfo.cntr_property = VT_PLUGIN_CNTR_ABS \
                    | VT_PLUGIN_CNTR_DOUBLE | VT_PLUGIN_CNTR_LAST;
#endif

#ifdef BACKEND_SCOREP
#define ENV_PREFIX "SCOREP_"
#define SET_METRIC_INFO(__minfo) \
            __minfo.description = NULL; \
            __minfo.mode        = SCOREP_METRIC_MODE_ACCUMULATED_LAST; \
            __minfo.value_type  = SCOREP_METRIC_VALUE_DOUBLE;          \
            __minfo.base        = SCOREP_METRIC_BASE_DECIMAL;          \
            __minfo.exponent    = 0;
#endif

#ifdef BACKEND_SCOREP
#include <scorep/SCOREP_MetricPlugins.h>
#endif
#ifdef BACKEND_VTRACE
#include <vampirtrace/vt_plugin_cntr.h>
#endif

#ifdef BACKEND_SCOREP
    typedef SCOREP_Metric_Plugin_MetricProperties metric_properties_t;
    typedef SCOREP_MetricTimeValuePair timevalue_t;
    typedef SCOREP_Metric_Plugin_Info plugin_info_type;
#endif

#ifdef BACKEND_VTRACE
    typedef vt_plugin_cntr_metric_info metric_properties_t;
    typedef vt_plugin_cntr_timevalue timevalue_t;
    typedef vt_plugin_cntr_info plugin_info_type;
#endif



/* to which cpu is the report thread pinned? (0==not pinned) */
static int pin_cpu=0;

static struct x86_energy_source *source = NULL;

static int nr_packages = -1;

union value {
    double dbl;
    uint64_t uint64;
};

struct event {
    int enabled;
    void* ID;
    char * name;
    int type;
    int ident;
    int package_nr;
    union value *reg_values;
}__attribute__((aligned(64)));

static int32_t event_list_size = 0;
static struct event event_list[512];
static uint64_t *timestamps;
static uint64_t sample_count = 0;

/* only used when keep file handles is true length is given by nr_sockets */

static int counter_enabled = 0;

static pthread_t thread;

static int interval_us = 100000;

static uint64_t (*wtime)(void) = NULL;

#define DEFAULT_BUF_SIZE (size_t)(4*1024*1024)
static size_t buf_size = DEFAULT_BUF_SIZE; // 4MB per Event per Thread

static int synchronous = 0;

static size_t parse_buffer_size(const char *s)
{
    char *tmp = NULL;
    size_t size;

    // parse number part
    size = strtoll(s, &tmp, 10);

    if (size == 0) {
        fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to parse buffer size ('%s'), using default %zu\n", s, DEFAULT_BUF_SIZE);
        return DEFAULT_BUF_SIZE;
    }

    // skip whitespace characters
    while (*tmp == ' ') tmp++;

    switch (*tmp) {
        case 'g':
        case 'G':
            size *= 1024;
        case 'm':
        case 'M':
            size *= 1024;
        case 'k':
        case 'K':
            size *= 1024;
        default:
            break;
    }

    return size;
}


static int init_devices(void)
{
    int i;
    /* init x86_energy */
    source = get_available_sources();
    if(source == NULL) {
        fprintf(stderr,"X86_ENERGY_PLUGIN: Could not detect a suitable cpu (errno = %i: %s)\n", errno, strerror(errno));
        return -1;
    }
    nr_packages = source->get_nr_packages();

    for(i=0;i<nr_packages;i++) {
        int ret;
        if ((ret = source->init_device(i)) != 0)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to initialize device %i of %i: %i (%s)!\n", i, nr_packages, ret, strerror(errno));
            return -1;
        }
    }

    return 0;
}

static void fini_devices(void)
{
    if (source != NULL) {
        int i;
        for (i=0;i<nr_packages;i++) {
            source->fini_device(i);
        }
        source->fini();
        source = NULL;
    }
}


/* interaction with VampirTrace */
static void set_pform_wtime_function(uint64_t(*pform_wtime)(void)) {
    wtime = pform_wtime;
}

static int32_t init(void) {
    char * env;
    /* sampling rate */

    printf("init()\n");

    env = getenv(ENV_PREFIX"X86ENERGY_INTERVAL_US");
    if (env == NULL) {
        interval_us = 100000;
    }
    else {
        interval_us = atoi(env);
        if (interval_us == 0) {
            fprintf(stderr,
                    "X86_ENERGY_PLUGIN: Could not parse X86ENERGY_INTERVAL_US, using 100 ms\n");
            interval_us = 100000;
        }
    }
    /* use nth cpu to read msrs */
    env = getenv(ENV_PREFIX"X86ENERGY_CPU_PIN");
    if (env != NULL) {
        pin_cpu = atoi(env);
    }

    env = getenv(ENV_PREFIX"X86ENERGY_BUF_SIZE");
    if (env != NULL) {
        buf_size = parse_buffer_size(env);
          if (buf_size < 1024)
          {
                fprintf(stderr, "X86_ENERGY_PLUGIN: Given buffer size (%zu) too small, falling back to default (%zu)\n", buf_size, DEFAULT_BUF_SIZE);
                buf_size = DEFAULT_BUF_SIZE;
          }
    }

    env = getenv(ENV_PREFIX"X86ENERGY_SYNCHRONOUS");
    if (env != NULL) {
        synchronous = (strcmp(env, "yes") == 0 || strcmp(env, "1") == 0 || strcmp(env, "true") == 0) ? 1 : 0;
    }

    return 0;
}

static metric_properties_t * get_event_info(char * event_name) {
    int i,j,k;
    char buffer[256];

    if (source == NULL) {
        if (init_devices() != 0){
            metric_properties_t * return_values = calloc(1, sizeof(metric_properties_t));
            return return_values;
        }
    }

    if (strcmp(event_name, "*") == 0 || strcmp(event_name, "*_power") == 0) {
        int numberOfFeatures = source->plattform_features->num;
        int numberOfMetrics = 2 + ((nr_packages+1)*numberOfFeatures); // NULL,syspower + ((nr_packages+sum_counter) * #features)
        j = 0;
        metric_properties_t * return_values;
        return_values = malloc(numberOfMetrics * sizeof(metric_properties_t));
        if (return_values == NULL) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space for counter information\n");
            return NULL;
        }

        for (k=0;k<numberOfFeatures;k++) {
            for (i=0;i<nr_packages;i++) {
                    snprintf(buffer, sizeof(buffer), "package%i_%s_power", i, source->plattform_features->name[k]);
                    return_values[j].name = strdup(buffer);
                    return_values[j].unit = strdup("Watt");
                    SET_METRIC_INFO(return_values[j]);
                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=POWER;
                    event_list[event_list_size].ident=source->plattform_features->ident[k];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                    j++;
            }
            snprintf(buffer, sizeof(buffer), "sum_%s_power", source->plattform_features->name[k]);
            return_values[j].name = strdup(buffer);
            return_values[j].unit = strdup("Watt");
            SET_METRIC_INFO(return_values[j]);
            event_list[event_list_size].name=strdup(buffer);
            event_list[event_list_size].type=SYSPOWER;
            event_list[event_list_size].ident=source->plattform_features->ident[k];
            event_list_size++;
            j++;
        }
        return_values[j].name = NULL;
        return return_values;
    }
    else if (strcmp(event_name, "*_energy") == 0) {
        int numberOfFeatures = source->plattform_features->num;
        int numberOfMetrics = 2 + ((nr_packages+1)*numberOfFeatures); // NULL,sysenergy + ((nr_packages+sum_counter) * #features)
        j = 0;
        metric_properties_t * return_values;
        return_values = malloc(numberOfMetrics * sizeof(metric_properties_t));
        if (return_values == NULL) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space for counter information\n");
            return NULL;
        }

        for (k=0;k<numberOfFeatures;k++) {
            for (i=0;i<nr_packages;i++) {
                    snprintf(buffer, sizeof(buffer), "package%i_%s_energy", i, source->plattform_features->name[k]);
                    return_values[j].name = strdup(buffer);
                    return_values[j].unit = strdup("Joule");
                    SET_METRIC_INFO(return_values[j]);
                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=ENERGY;
                    event_list[event_list_size].ident=source->plattform_features->ident[k];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                    j++;
            }
            snprintf(buffer, sizeof(buffer), "sum_%s_energy", source->plattform_features->name[k]);
            return_values[j].name = strdup(buffer);
            return_values[j].unit = strdup("Joule");
            SET_METRIC_INFO(return_values[j]);
            event_list[event_list_size].name=strdup(buffer);
            event_list[event_list_size].type=SYSENERGY;
            event_list[event_list_size].ident=source->plattform_features->ident[k];
            event_list_size++;
            j++;
        }
        return_values[j].name = NULL;
        return return_values;
    }
    /* plattform specific features */
    else {
        int numberOfFeatures = source->plattform_features->num;
        for (j=0;j<numberOfFeatures;j++) {
            char * feature_name = source->plattform_features->name[j];
            int feature_name_len = strlen(feature_name);
            /* find the matching feature and make sure the event name syntax is correct (<feature>_[power|energy]) */
            if(strncmp(event_name, feature_name, feature_name_len) == 0 &&
               ( strcmp(&event_name[feature_name_len], "_power") == 0 || strcmp(&event_name[feature_name_len], "_energy") == 0) ) {
                int type;
                metric_properties_t * return_values;
                return_values = malloc((nr_packages + 2) * sizeof(metric_properties_t));
                if (return_values == NULL) {
                    fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space for counter information\n");
                    return NULL;
                }

                /* check if it is an energy counter
                 * otherwise assume it is a power counter */
                if (strstr(event_name, "energy"))
                    type = ENERGY;
                else
                    type = POWER;

                /* counter for every package */
                for (i = 0; i < nr_packages; i++) {
                    if (type == ENERGY) {
                        snprintf(buffer, sizeof(buffer), "socket%d_%s_energy", i, feature_name);
                        return_values[i].name = strdup(buffer);
                        return_values[i].unit = strdup("Joule");
                        SET_METRIC_INFO(return_values[i]);
                    }
                    else {
                        snprintf(buffer, sizeof(buffer), "socket%d_%s_power", i, feature_name);
                        return_values[i].name = strdup(buffer);
                        return_values[i].unit = strdup("Watt");
                        SET_METRIC_INFO(return_values[i]);
                    }

                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=type;
                    event_list[event_list_size].ident=source->plattform_features->ident[j];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                }

                /* sum of all package counter */
                if (type == ENERGY) {
                    snprintf(buffer, sizeof(buffer), "sum_%s_energy", feature_name);
                    return_values[nr_packages].name = strdup(buffer);
                    return_values[nr_packages].unit = strdup("Joule");
                    SET_METRIC_INFO(return_values[nr_packages]);
                    type = SYSENERGY;
                }
                else {
                    snprintf(buffer, sizeof(buffer), "sum_%s_power", feature_name);
                    return_values[nr_packages].name = strdup(buffer);
                    return_values[nr_packages].unit = strdup("Watt");
                    SET_METRIC_INFO(return_values[nr_packages]);
                    type = SYSPOWER;
                }

                event_list[event_list_size].name=strdup(buffer);
                event_list[event_list_size].type=type;
                event_list[event_list_size].ident=source->plattform_features->ident[j];
                event_list_size++;
                /* if the description is null it should be considered the end */
                /* Last element empty */
                return_values[nr_packages+1].name = NULL;
                return return_values;
            }
        }
    }

    fprintf(stderr, "X86_ENERGY_PLUGIN: Unknown Event %s\n", event_name);
    return NULL;
}

static void * thread_report(void * ignore) {
    uint64_t timestamp, timestamp2;
    int i;
    uint64_t num_entries = buf_size / sizeof(union value);

    if (pin_cpu) {
        /* compute where to pin the thread */
        cpu_set_t cpu_mask;
        CPU_ZERO(&cpu_mask);
        CPU_SET(pin_cpu, &cpu_mask);
        sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask);
    }

    counter_enabled = 1;
    while (counter_enabled) {
        if (wtime == NULL)
            return NULL;
        if (sample_count >= num_entries) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: buffer size of %zu is to small.\n", buf_size);
            fprintf(stderr, "X86_ENERGY_PLUGIN: Increase the buffer size with the environment variable %sX86ENERGY_BUF_SIZE\n",ENV_PREFIX);
            fprintf(stderr, "X86_ENERGY_PLUGIN: Stopping sample thread\n");
            return NULL;
        }

         /* get time */
        timestamp = wtime();
         /* measure */
        for (i = 0; i < event_list_size; i++) {
            if (event_list[i].enabled) {
                if(event_list[i].type==ENERGY)
                    event_list[i].reg_values[sample_count].dbl = source->get_energy(event_list[i].package_nr, event_list[i].ident);
                else if (event_list[i].type==POWER)
                    event_list[i].reg_values[sample_count].dbl = source->get_power(event_list[i].package_nr, event_list[i].ident);
            }
        }
        /* get time */
        timestamp2 = wtime();
        /* avg */
        timestamps[sample_count] = timestamp + ((timestamp2 - timestamp) >> 1);

        sample_count++;
        usleep(interval_us);
    }

    return NULL;
}

static int is_sorted = 0;
static int is_thread_created = 0;

static int32_t add_counter(char * event_name) {
    int i;
    size_t num_entries = buf_size / sizeof(union value);

    /* initialize */
    if (source == NULL)
    {
        if (init_devices() != 0)
            return -1;
    }

    /* sorting of events to prevent thread bouncing between multiple sockets */
    if(!is_sorted) {
        struct event tmp;
        for(i=event_list_size;i>1;i--) {
            int j;
            for(j=0; j<i-1; j++) {
                if (event_list[j].package_nr > event_list[j+1].package_nr) {
                    memcpy(&tmp, &event_list[j], sizeof(struct event));
                    memcpy(&event_list[j], &event_list[j+1], sizeof(struct event));
                    memcpy(&event_list[j+1], &tmp, sizeof(struct event));
                }
            }
        }
        is_sorted=1;
    }

    if(!is_thread_created && !synchronous) {
        fprintf(stderr, "X86_ENERGY_PLUGIN: using buffer with %zu entries per feature\n", num_entries);
        /* allocate space for time values */
        timestamps = malloc(num_entries * sizeof(uint64_t));
        if (timestamps == NULL)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to allocate memory for timestamps (%zu B)\n", num_entries * sizeof(uint64_t));
            return -1;
        }
        if (pthread_create(&thread, NULL, &thread_report, NULL) != 0)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to create measurement thread!\n");
            return -1;
        }

        is_thread_created = 1;
    }

    for(i=0;i<event_list_size;i++) {
        if(!strcmp(event_name, event_list[i].name)) {
            /* only space for !sum counter has to be allocated in advance */
            if (strncmp(event_name, "sum", 3) == 0) {
               event_list[i].enabled = 0;
            }
            else {
                if (!synchronous) {
                    event_list[i].reg_values = malloc(num_entries * sizeof(union value));
                    if (event_list[i].reg_values == NULL)
                    {
                        fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to allocate memory for reg_values (%zu B)\n", num_entries * sizeof(union value));
                        return -1;
                    }
                } else {
                    event_list[i].reg_values = NULL;
                }
                event_list[i].enabled = 1;
            }
            return i;
        }
    }
    /* return id */
    return -1;
}

static void fini(void) {
    if (counter_enabled)
    {
        counter_enabled = 0;
        if (!synchronous) {
            pthread_join(thread, NULL);
        }
    }
    fini_devices();
}


/*bool
get_value( int32_t   counterIndex,
           uint64_t* value )
           */
uint64_t
get_value(int32_t counterIndex)
{
    printf("get_value()\n");
    /* TODO: How to determine whether the value is new? Signal "no new value" if  not */
    union value val;
    if (event_list[counterIndex].enabled) {
        if(event_list[counterIndex].type==ENERGY) {
            val.dbl = source->get_energy(event_list[counterIndex].package_nr, event_list[counterIndex].ident);
        } else if (event_list[counterIndex].type==POWER) {
            val.dbl = source->get_power(event_list[counterIndex].package_nr, event_list[counterIndex].ident);
        } else {
            val.dbl = 0.0;
        }
        //*value = val.uint64;
        //return true;
        return val.uint64;
    }
    //return false;
    return 0;

}

static uint64_t get_all_values(int32_t id, timevalue_t **result)
{
    uint64_t i;
    int32_t j;
    timevalue_t *res;

    if (counter_enabled)
    {
        counter_enabled = 0;
        pthread_join(thread, NULL);
    }

    res = (timevalue_t*) calloc(sample_count, sizeof(timevalue_t));

    for (i = 0; i < sample_count; i++) {
        /* create the sum */
    int sumtype = -1;
        if (event_list[id].type == SYSENERGY)
            sumtype = ENERGY;
        else if (event_list[id].type == SYSPOWER)
            sumtype = POWER;

        if (sumtype != -1) {
            union value sum;
            sum.dbl = 0;
            for (j=0;j<event_list_size;j++) {
                if (event_list[j].type == sumtype &&
                    event_list[j].ident == event_list[id].ident)
                {
                    sum.dbl += event_list[j].reg_values[i].dbl;
                }
            }
            res[i].value = sum.uint64;
        }
        else {
            res[i].value = event_list[id].reg_values[i].uint64;
        }
        res[i].timestamp = timestamps[i];
    }
    *result = res;

    return sample_count;
}

#ifdef BACKEND_SCOREP
SCOREP_METRIC_PLUGIN_ENTRY( x86energy_plugin )
#endif
#ifdef BACKEND_VTRACE
vt_plugin_cntr_info get_info()
#endif
{
    plugin_info_type info;
    memset(&info, 0, sizeof(plugin_info_type));

    const char *env = getenv(ENV_PREFIX"X86ENERGY_SYNCHRONOUS");
    if (env != NULL) {
        synchronous = (strcmp(env, "yes") == 0 || strcmp(env, "1") == 0 || strcmp(env, "true") == 0) ? 1 : 0;
    }


#ifdef BACKEND_VTRACE
    info.init = init;
    info.set_pform_wtime_function = set_pform_wtime_function;
    info.vt_plugin_cntr_version = VT_PLUGIN_CNTR_VERSION;
    info.run_per = VT_PLUGIN_CNTR_PER_HOST;
    info.synch = VT_PLUGIN_CNTR_ASYNCH_POST_MORTEM;
#endif

#ifdef BACKEND_SCOREP
    info.plugin_version               = SCOREP_METRIC_PLUGIN_VERSION;
    info.run_per                      = SCOREP_METRIC_PER_HOST;
    if (synchronous) {
        info.sync                     = SCOREP_METRIC_SYNC;
        printf("Setting plugin as synchronous\n");
    } else {
        info.sync                     = SCOREP_METRIC_ASYNC;
    }
    info.delta_t                      = UINT64_MAX;
    info.initialize                   = init;
    info.set_clock_function           = set_pform_wtime_function;
#endif

    info.finalize = fini;
    info.add_counter = add_counter;
    if (synchronous) {
        info.get_current_value = get_value;
    } else {
        info.get_all_values = get_all_values;
    }
    info.get_event_info = get_event_info;
    return info;
}
