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
#include <sys/time.h> //Ask: Needed? Score-P?
#include <time.h> // time measurement without scorep
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h> //toupper

#include <x86_energy.h>

#define ENERGY 0
#define POWER  1
#define SUMENERGY 2
#define SUMPOWER 3
#define SYSENERGY 4
#define SYSPOWER 5

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
#define SET_METRIC_INFO_ACC(__minfo) \
            __minfo.cntr_property = VT_PLUGIN_CNTR_ACC \
                    | VT_PLUGIN_CNTR_DOUBLE | VT_PLUGIN_CNTR_LAST;
#define SET_METRIC_INFO_ABS(__minfo) \
            __minfo.cntr_property = VT_PLUGIN_CNTR_ABS \
                    | VT_PLUGIN_CNTR_DOUBLE | VT_PLUGIN_CNTR_LAST;
#endif

#ifdef BACKEND_SCOREP
#define ENV_PREFIX "SCOREP_"
#define SET_METRIC_INFO_ACC(__minfo) \
            __minfo.description = NULL; \
            __minfo.mode        = SCOREP_METRIC_MODE_ACCUMULATED_LAST; \
            __minfo.value_type  = SCOREP_METRIC_VALUE_DOUBLE;          \
            __minfo.base        = SCOREP_METRIC_BASE_DECIMAL;          \
            __minfo.exponent    = 0;
#define SET_METRIC_INFO_ABS(__minfo) \
            __minfo.description = NULL; \
            __minfo.mode        = SCOREP_METRIC_MODE_ABSOLUTE_LAST; \
            __minfo.value_type  = SCOREP_METRIC_VALUE_DOUBLE;          \
            __minfo.base        = SCOREP_METRIC_BASE_DECIMAL;          \
            __minfo.exponent    = 0;
#endif

#define PLUGIN_PREFIX "METRIC_X86_ENERGY_PLUGIN_"

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

// default buffer size for char buffer
#define BUFFERSIZE 256


/* to which cpu is the report thread pinned? (0==not pinned) */
static int pin_cpu=0;

static struct x86_energy_source *source = NULL;

static int nr_packages = -1;
static int numberOfFeatures = -1;

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

struct timestamp_scorep_gettime
{
    uint64_t scorep;
    double millisecs;
};

static int32_t event_list_size = 0;
static struct event event_list[512];
static struct timestamp_scorep_gettime *timestamps;
static uint64_t sample_count = 0;

/* only used when keep file handles is true length is given by nr_sockets */

static int counter_enabled = 0;

static pthread_t thread;

static int interval_us = 100000;

static uint64_t (*wtime)(void) = NULL;

#define DEFAULT_BUF_SIZE (size_t)(4*1024*1024)
static size_t buf_size = DEFAULT_BUF_SIZE; // 4MB per Event per Thread

static int synchronous = 0;

/* power offset for the blade */
static double offset = 0;

static size_t parse_buffer_size(const char *s)
{
    char *tmp = NULL;
    size_t size;

    // parse number part
    size = strtoll(s, &tmp, 10);

    if (size == 0) {
        fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to parse buffer size ('%s'), "\
                "using default %zu\n", s, DEFAULT_BUF_SIZE);
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
        fprintf(stderr,"X86_ENERGY_PLUGIN: Could not detect a suitable cpu "\
                "(errno = %i: %s)\n", errno, strerror(errno));
        return -1;
    }
    nr_packages = source->get_nr_packages();
    numberOfFeatures = source->plattform_features->num;

    for(i=0;i<nr_packages;i++) {
        int ret;
        if ((ret = source->init_device(i)) != 0)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to initialize device "\
                    "%i of %i: %i (%s)!\n", i, nr_packages, ret, strerror(errno));
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

/* change the name to uppercase and put the result in the given buffer */
void strupr(char * name, char * buffer)
{
    int i;

    if (strlen(name) >= BUFFERSIZE)
        fprintf(stderr, "X86_ENERGY_PLUGIN: %s is too long for the buffer. "\
                "Please increase char buffer size to get correct sensor names.\n");
    
    for(i = 0; i <= strlen(name) && i < BUFFERSIZE - 1; i++)
    {
        buffer[i] = toupper(name[i]);
    }

    buffer[i+1] = '\0';
}

static int32_t init(void) {
    char * env;
    /* sampling rate */

    printf("init()\n");

    env = getenv(ENV_PREFIX PLUGIN_PREFIX "INTERVAL_US");
    if (env == NULL) {
        interval_us = 100000;
    }
    else {
        interval_us = atoi(env);
        if (interval_us == 0) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Could not parse "\
                    "%sINTERVAL_US, using 100 ms\n", ENV_PREFIX PLUGIN_PREFIX);
            interval_us = 100000;
        }
    }
    /* use nth cpu to read msrs */
    env = getenv(ENV_PREFIX PLUGIN_PREFIX "CPU_PIN");
    if (env != NULL) {
        pin_cpu = atoi(env);
    }

    env = getenv(ENV_PREFIX PLUGIN_PREFIX "BUF_SIZE");
    if (env != NULL) {
        buf_size = parse_buffer_size(env);
          if (buf_size < 1024)
          {
              fprintf(stderr, "X86_ENERGY_PLUGIN: Given buffer size (%zu) "\
                      "too small, falling back to default (%zu)\n", buf_size, \
                      DEFAULT_BUF_SIZE);
                buf_size = DEFAULT_BUF_SIZE;
          }
    }

    env = getenv(ENV_PREFIX PLUGIN_PREFIX "SYNCHRONOUS");
    if (env != NULL) {
        synchronous = (strcmp(env, "yes") == 0 || strcmp(env, "1") == 0 || \
                strcmp(env, "true") == 0) ? 1 : 0;
    }

    /* offset in mW for the blade */
    env = getenv(ENV_PREFIX PLUGIN_PREFIX "OFFSET");
    if (env != NULL)
    {
        offset = atof(env);
        if (offset < 0)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Power offset can't be negative "\
                    "setting value to zero\n");
            offset = 0;
        }
    }

    return 0;
}

static metric_properties_t * get_event_info(char * event_name) {
    printf("get_event_info()\n");
    int i,j,k;
    char buffer[BUFFERSIZE];
    char feature_name[BUFFERSIZE];

    if (source == NULL) {
        if (init_devices() != 0){
            metric_properties_t * return_values = calloc(1, 
                    sizeof(metric_properties_t));
            return return_values;
        }
    }

    if (strcmp(event_name, "*/P") == 0) {
        // NULL,syspower + ((nr_packages+sum_counter) * #features)
        int numberOfMetrics = 2 + ((nr_packages+1)*numberOfFeatures);
        j = 0;
        metric_properties_t * return_values;
        return_values = malloc(numberOfMetrics * sizeof(metric_properties_t));
        if (return_values == NULL) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space for "\
                    "counter information\n");
            return NULL;
        }

        for (k=0;k<numberOfFeatures;k++) {
            strupr(source->plattform_features->name[k], feature_name);
            for (i=0;i<nr_packages;i++) {
                    snprintf(buffer, sizeof(buffer), "%s%i/P", feature_name, i);
                    return_values[j].name = strdup(buffer);
                    return_values[j].unit = strdup("mW");
                    SET_METRIC_INFO_ABS(return_values[j]);
                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=POWER;
                    event_list[event_list_size].ident=
                        source->plattform_features->ident[k];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                    j++;
            }
            snprintf(buffer, sizeof(buffer), "%s_SUM/P", feature_name);
            return_values[j].name = strdup(buffer);
            return_values[j].unit = strdup("Watt");
            SET_METRIC_INFO_ABS(return_values[j]);
            event_list[event_list_size].name=strdup(buffer);
            event_list[event_list_size].type=SUMPOWER;
            event_list[event_list_size].ident=source->plattform_features->ident[k];
            event_list_size++;
            j++;
        }
        snprintf(buffer, sizeof(buffer), "%s/P", "BLADE");
        return_values[j].name = strdup(buffer);
        return_values[j].unit = strdup("mW");
        SET_METRIC_INFO_ABS(return_values[j]);
        event_list[event_list_size].name=strdup(buffer);
        event_list[event_list_size].type=SYSPOWER;
        event_list[event_list_size].ident=-1;
        event_list_size++;
        j++;

        return_values[j].name = NULL;
        return return_values;
    }
    else if (strcmp(event_name, "*") == 0 || strcmp(event_name, "*/E") == 0) {
        // NULL,sysenergy + ((nr_packages+sum_counter) * #features)
        int numberOfMetrics = 2 + ((nr_packages+1)*numberOfFeatures);
        j = 0;
        metric_properties_t * return_values;
        return_values = malloc(numberOfMetrics * sizeof(metric_properties_t));
        if (return_values == NULL) {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space for "\
                    "counter information\n");
            return NULL;
        }

        for (k=0;k<numberOfFeatures;k++) {
            strupr(source->plattform_features->name[k], feature_name);
            for (i=0;i<nr_packages;i++) {
                    snprintf(buffer, sizeof(buffer), "%s%i/E", feature_name, i);
                    return_values[j].name = strdup(buffer);
                    return_values[j].unit = strdup("mJ");
                    SET_METRIC_INFO_ACC(return_values[j]);
                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=ENERGY;
                    event_list[event_list_size].ident=
                        source->plattform_features->ident[k];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                    j++;
            }
            snprintf(buffer, sizeof(buffer), "%s_SUM/E", feature_name);
            return_values[j].name = strdup(buffer);
            return_values[j].unit = strdup("mJ");
            SET_METRIC_INFO_ACC(return_values[j]);
            event_list[event_list_size].name=strdup(buffer);
            event_list[event_list_size].type=SUMENERGY;
            event_list[event_list_size].ident=source->plattform_features->ident[k];
            event_list_size++;
            j++;
        }
        snprintf(buffer, sizeof(buffer), "%s/E", "BLADE");
        return_values[j].name = strdup(buffer);
        return_values[j].unit = strdup("mJ");
        SET_METRIC_INFO_ABS(return_values[j]);
        event_list[event_list_size].name=strdup(buffer);
        event_list[event_list_size].type=SYSENERGY;
        event_list[event_list_size].ident=-1;
        event_list_size++;
        j++;

        return_values[j].name = NULL;
        return return_values;
    }
    /* plattform specific features */
    else {
        for (j=0;j<numberOfFeatures;j++) {
            strupr(source->plattform_features->name[j], feature_name);
            int feature_name_len = strlen(feature_name);
            /* find the matching feature and make sure the event name 
             * syntax is correct (<FEATURE>/[E|P]) */
            if(strncmp(event_name, feature_name, feature_name_len) == 0 &&
                ( strcmp(&event_name[feature_name_len], "/P") == 0 || \
                  strcmp(&event_name[feature_name_len], "/E") == 0) ) {
                int type;
                metric_properties_t * return_values;
                return_values = malloc((nr_packages + 2) * sizeof(metric_properties_t));
                if (return_values == NULL) {
                    fprintf(stderr, "X86_ENERGY_PLUGIN: Unable to allocate space "\
                            "for counter information\n");
                    return NULL;
                }

                /* check if it is an energy counter
                 * otherwise assume it is a power counter */
                if (strstr(event_name, "/E"))
                    type = ENERGY;
                else
                    type = POWER;

                /* counter for every package */
                for (i = 0; i < nr_packages; i++) {
                    if (type == ENERGY) {
                        snprintf(buffer, sizeof(buffer), "%s%d/E", feature_name, i);
                        return_values[i].name = strdup(buffer);
                        return_values[i].unit = strdup("Joule");
                        SET_METRIC_INFO_ACC(return_values[i]);
                    }
                    else {
                        snprintf(buffer, sizeof(buffer), "%s%d/P", feature_name, i);
                        return_values[i].name = strdup(buffer);
                        return_values[i].unit = strdup("Watt");
                        SET_METRIC_INFO_ABS(return_values[i]);
                    }

                    event_list[event_list_size].name=strdup(buffer);
                    event_list[event_list_size].type=type;
                    event_list[event_list_size].ident=
                        source->plattform_features->ident[j];
                    event_list[event_list_size].package_nr=i;
                    event_list_size++;
                }

                /* sum of all package counter */
                if (type == ENERGY) {
                    snprintf(buffer, sizeof(buffer), "%s_SUM/E", feature_name);
                    return_values[nr_packages].name = strdup(buffer);
                    return_values[nr_packages].unit = strdup("Joule");
                    SET_METRIC_INFO_ACC(return_values[nr_packages]);
                    type = SUMENERGY;
                }
                else {
                    snprintf(buffer, sizeof(buffer), "%s_SUM/P", feature_name);
                    return_values[nr_packages].name = strdup(buffer);
                    return_values[nr_packages].unit = strdup("Watt");
                    SET_METRIC_INFO_ABS(return_values[nr_packages]);
                    type = SUMPOWER;
                }

                event_list[event_list_size].name=strdup(buffer);
                event_list[event_list_size].type=type;
                event_list[event_list_size].ident=
                    source->plattform_features->ident[j];
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
    struct timespec timespec, timespec2;
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
            fprintf(stderr, "X86_ENERGY_PLUGIN: buffer size of %zu is to small.\n", 
                    buf_size);
            fprintf(stderr, "X86_ENERGY_PLUGIN: Increase the buffer size with the "\
                    "environment variable %sSIZE\n",ENV_PREFIX PLUGIN_PREFIX);
            fprintf(stderr, "X86_ENERGY_PLUGIN: Stopping sample thread\n");
            return NULL;
        }

        /* get time, correct Score-P time is more important */
        clock_gettime(CLOCK_MONOTONIC, &timespec);
        timestamp = wtime();
        /* measure */
        for (i = 0; i < event_list_size; i++) {
            if (event_list[i].enabled) {
                if(event_list[i].type==ENERGY)
                    event_list[i].reg_values[sample_count].dbl = 
                        source->get_energy(event_list[i].package_nr, 
                                event_list[i].ident);
                else if (event_list[i].type==POWER)
                    event_list[i].reg_values[sample_count].dbl = 
                        source->get_power(event_list[i].package_nr, 
                                event_list[i].ident);
            }
        }
        /* get time */
        timestamp2 = wtime();
        clock_gettime(CLOCK_MONOTONIC, &timespec2);
        /* avg */
        timestamps[sample_count].scorep = timestamp + 
            ((timestamp2 - timestamp) >> 1);

        timestamps[sample_count].millisecs = timespec.tv_sec*1E3 + \
            timespec.tv_nsec/1E6 + ((timespec2.tv_sec - timespec.tv_sec)*1E3 + \
                    (timespec2.tv_nsec - timespec.tv_nsec)/1E6)/2.0;

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
        fprintf(stderr, "X86_ENERGY_PLUGIN: using buffer with %zu entries per "\
                "feature\n", num_entries);
        /* allocate space for time values */
        timestamps = malloc(num_entries * sizeof(struct timestamp_scorep_gettime));
        if (timestamps == NULL)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to allocate memory for "\
                    "timestamps (%zu B)\n", num_entries * 
                    sizeof(struct timestamp_scorep_gettime));
            return -1;
        }
        if (pthread_create(&thread, NULL, &thread_report, NULL) != 0)
        {
            fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to create measurement "\
                    "thread!\n");
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
                        fprintf(stderr, "X86_ENERGY_PLUGIN: Failed to allocate "\
                                "memory for reg_values (%zu B)\n", 
                                num_entries * sizeof(union value));
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
            val.dbl = source->get_energy(event_list[counterIndex].package_nr, 
                    event_list[counterIndex].ident);
        } else if (event_list[counterIndex].type==POWER) {
            val.dbl = source->get_power(event_list[counterIndex].package_nr, 
                    event_list[counterIndex].ident);
        } else {
            val.dbl = 0.0;
        }
        //*value = val.uint64;
        //return true;
        /* convert to mJ/mW */
        val.dbl *= 1000;
        return val.uint64;
    }
    //return false;
    return 0;

}

static uint64_t get_all_values(int32_t id, timevalue_t **result)
{
    uint64_t i;
    int32_t j;
    int sumtype = -1;
    int blade = -1;
    timevalue_t *res;
    union value first_result, sample_result;

    if (counter_enabled)
    {
        counter_enabled = 0;
        pthread_join(thread, NULL);
    }

    res = (timevalue_t*) calloc(sample_count, sizeof(timevalue_t));

    /* distinguish between the different sum forms */
    switch (event_list[id].type)
    {
        /* sum for one sensor over all packages */
        case SUMENERGY:
            sumtype = ENERGY;
            break;

        case SUMPOWER:
            sumtype = POWER;
            break;

        /* sum over all sensors on all packages */
        case SYSENERGY:
            sumtype = ENERGY;
            blade = 1;
            break;

        case SYSPOWER:
            sumtype = POWER;
            blade = 1;
            break;
    }

    for (i = 0; i < sample_count; i++)
    {
        /* create the sum if the sumtype was set previously */
        if (sumtype != -1)
        {
            union value sum;
            sum.dbl = 0;
            for (j=0;j<event_list_size;j++)
            {
                /* only do further work if the sumtype matches */
                if (event_list[j].type == sumtype)
                {
                    /* sum for one sensor over all packages */
                    if (blade == -1 && 
                            event_list[j].ident == event_list[id].ident)
                    {
                        sum.dbl += event_list[j].reg_values[i].dbl;
                    }
                    /* sum for the blade */
                    /* don't include CORE in sum because PACKAGE contains CORE */
                    else if (blade == 1 && 
                            strstr(event_list[j].name, "CORE") == NULL)
                    {
                        sum.dbl += event_list[j].reg_values[i].dbl;
                    }
                }
            }

            /* add the offset for the BLADE */
            if (blade == 1 && sumtype == ENERGY)
            {
                /* convertation to J is needed for rapl */
                sum.dbl += offset/1000.0 * (timestamps[i].millisecs - \
                        timestamps[0].millisecs)/1000.0;
            }
            else if (blade == 1 && sumtype == POWER)
            {
                /* this value has to be in W because of rapl */
                sum.dbl += offset/1000.0;
            }

            sample_result.uint64 = sum.uint64;
        }
        else {
            sample_result.uint64 = event_list[id].reg_values[i].uint64;
        }

        if (i == 0)
            /* save first result for rescaling */
            first_result.dbl = sample_result.dbl;

        /* rescale with first result and convert to mJ/mW
         * first result is the zero value */
        sample_result.dbl = (sample_result.dbl - first_result.dbl)*1000;

        /* ugly work because scorep transforms the uint64_t
         * internally to double */
        res[i].value = sample_result.uint64;

        res[i].timestamp = timestamps[i].scorep;
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

    const char *env = getenv(ENV_PREFIX PLUGIN_PREFIX "SYNCHRONOUS");
    if (env != NULL) {
        synchronous = (strcmp(env, "yes") == 0 || strcmp(env, "1") == 0 || \
                strcmp(env, "true") == 0) ? 1 : 0;
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
