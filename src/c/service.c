/*
 * Copyright (c) 2018
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "service.h"
#include "device.h"
#include "discovery.h"
#include "callback.h"
#include "metrics.h"
#include "errorlist.h"
#include "rest-server.h"
#include "profiles.h"
#include "metadata.h"
#include "data.h"
#include "rest.h"
#include "edgex-rest.h"
#include "iot/time.h"
#include "iot/iot.h"
#include "edgex/csdk-defs.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/utsname.h>

#include <microhttpd.h>

#define EDGEX_DEV_API_PING "/api/v1/ping"
#define EDGEX_DEV_API_VERSION "/api/version"
#define EDGEX_DEV_API_DISCOVERY "/api/v1/discovery"
#define EDGEX_DEV_API_DEVICE "/api/v1/device/"
#define EDGEX_DEV_API_CALLBACK "/api/v1/callback"
#define EDGEX_DEV_API_CONFIG "/api/v1/config"
#define EDGEX_DEV_API_METRICS "/api/v1/metrics"

#define POOL_THREADS 8

typedef struct postparams
{
  devsdk_service_t *svc;
  edgex_event_cooked *event;
} postparams;

void devsdk_usage ()
{
  printf ("  -n, --name=<name>\t: Set the device service name\n");
  printf ("  -r, --registry=<url>\t: Use the registry service\n");
  printf ("  -p, --profile=<name>\t: Set the profile name\n");
  printf ("  -c, --confdir=<dir>\t: Set the configuration directory\n");
}

static bool testArgOpt (const char *arg, const char *val, const char *pshort, const char *plong, const char **var, bool *result)
{
  if (strcmp (arg, pshort) == 0 || strcmp (arg, plong) == 0)
  {
    if (val && *val && *val != '-')
    {
      *var = val;
    }
    else
    {
      if (*var == NULL)
      {
        *var = "";
      }
      *result = false;
    }
    return true;
  }
  else
  {
    return false;
  }
}

static bool testArg (const char *arg, const char *val, const char *pshort, const char *plong, const char **var, bool *result)
{
  if (strcmp (arg, pshort) == 0 || strcmp (arg, plong) == 0)
  {
    if (val && *val)
    {
      *var = val;
    }
    else
    {
      printf ("Option \"%s\" requires a parameter\n", arg);
      *result = false;
    }
    return true;
  }
  else
  {
    return false;
  }
}

static void consumeArgs (int *argc_p, char **argv, int start, int nargs)
{
  for (int n = start + nargs; n < *argc_p; n++)
  {
    argv[n - nargs] = argv[n];
  }
  *argc_p -= nargs;
}

static bool processCmdLine (int *argc_p, char **argv, devsdk_service_t *svc)
{
  bool result = true;
  char *eq;
  const char *arg;
  const char *val;
  int argc = *argc_p;

  val = getenv ("edgex_registry");
  if (val)
  {
    svc->regURL = val;
  }

  int n = 1;
  while (result && n < argc)
  {
    arg = argv[n];
    val = NULL;
    eq = strchr (arg, '=');
    if (eq)
    {
      *eq = '\0';
      val = eq + 1;
    }
    else if (n + 1 < argc)
    {
      val = argv[n + 1];
    }
    if (testArgOpt (arg, val, "-r", "--registry", &svc->regURL, &result))
    {
      consumeArgs (&argc, argv, n, result ? 2 : 1);
      result = true;
    } else if
    (
      testArg (arg, val, "-n", "--name", &svc->name, &result) ||
      testArg (arg, val, "-p", "--profile", &svc->profile, &result) ||
      testArg (arg, val, "-c", "--confdir", &svc->confdir, &result)
    )
    {
      consumeArgs (&argc, argv, n, eq ? 1 : 2);
    }
    else
    {
      n++;
    }
    if (eq)
    {
      *eq = '=';
    }
  }
  *argc_p = argc;
  return result;
}

devsdk_service_t *devsdk_service_new
  (const char *defaultname, const char *version, void *impldata, devsdk_callbacks implfns, int *argc, char **argv, devsdk_error *err)
{
  if (impldata == NULL)
  {
    iot_log_error
      (iot_logger_default (), "devsdk_service_new: no implementation object");
    *err = EDGEX_NO_DEVICE_IMPL;
    return NULL;
  }
  if (defaultname == NULL || strlen (defaultname) == 0)
  {
    iot_log_error
      (iot_logger_default (), "devsdk_service_new: no default name specified");
    *err = EDGEX_NO_DEVICE_NAME;
    return NULL;
  }
  if (version == NULL || strlen (version) == 0)
  {
    iot_log_error
      (iot_logger_default (), "devsdk_service_new: no version specified");
    *err = EDGEX_NO_DEVICE_VERSION;
    return NULL;
  }

  *err = EDGEX_OK;
  devsdk_service_t *result = malloc (sizeof (devsdk_service_t));
  memset (result, 0, sizeof (devsdk_service_t));

  if (!processCmdLine (argc, argv, result))
  {
    *err = EDGEX_INVALID_ARG;
    return NULL;
  }

  if (result->name == NULL)
  {
    result->name = defaultname;
  }
  if (result->confdir == NULL)
  {
    result->confdir = "res";
  }
  result->version = version;
  result->userdata = impldata;
  result->userfns = implfns;
  result->devices = edgex_devmap_alloc (result);
  result->watchlist = edgex_watchlist_alloc ();
  result->logger = iot_logger_alloc_custom (result->name, IOT_LOG_TRACE, "-", edgex_log_tofile, NULL, true);
  result->thpool = iot_threadpool_alloc (POOL_THREADS, 0, -1, -1, result->logger);
  result->scheduler = iot_scheduler_alloc (-1, -1, result->logger);
  pthread_mutex_init (&result->discolock, NULL);
  return result;
}

static int ping_handler
(
  void *ctx,
  char *url,
  const devsdk_nvpairs *qparams,
  edgex_http_method method,
  const char *upload_data,
  size_t upload_data_size,
  void **reply,
  size_t *reply_size,
  const char **reply_type
)
{
  devsdk_service_t *svc = (devsdk_service_t *) ctx;
  *reply = strdup (svc->version);
  *reply_size = strlen (svc->version);
  *reply_type = "text/plain";
  return MHD_HTTP_OK;
}

static int version_handler
(
  void *ctx,
  char *url,
  const devsdk_nvpairs *qparams,
  edgex_http_method method,
  const char *upload_data,
  size_t upload_data_size,
  void **reply,
  size_t *reply_size,
  const char **reply_type
)
{
  devsdk_service_t *svc = (devsdk_service_t *) ctx;
  JSON_Value *val = json_value_init_object ();
  JSON_Object *obj = json_value_get_object (val);
  json_object_set_string (obj, "version", svc->version);
  json_object_set_string (obj, "sdk_version", CSDK_VERSION_STR);
  *reply = json_serialize_to_string (val);
  *reply_size = strlen (*reply);
  *reply_type = "application/json";
  json_value_free (val);
  return MHD_HTTP_OK;
}

static bool ping_client
(
  iot_logger_t *lc,
  const char *sname,
  edgex_device_service_endpoint *ep,
  int retries,
  struct timespec delay,
  devsdk_error *err
)
{
  edgex_ctx ctx;
  char url[URL_BUF_SIZE];

  if (ep->host == NULL || ep->port == 0)
  {
    iot_log_error (lc, "Missing endpoint for %s service.", sname);
    *err = EDGEX_BAD_CONFIG;
    return false;
  }

  snprintf (url, URL_BUF_SIZE - 1, "http://%s:%u/api/v1/ping", ep->host, ep->port);

  do
  {
    memset (&ctx, 0, sizeof (edgex_ctx));
    edgex_http_get (lc, &ctx, url, NULL, err);
    if (err->code == 0)
    {
      iot_log_info (lc, "Found %s service at %s:%d", sname, ep->host, ep->port);
      return true;
    }
  } while (retries-- && nanosleep (&delay, NULL) == 0);

  iot_log_error (lc, "Can't connect to %s service at %s:%d", sname, ep->host, ep->port);
  *err = EDGEX_REMOTE_SERVER_DOWN;
  return false;
}

static void startConfigured (devsdk_service_t *svc, toml_table_t *config, devsdk_error *err)
{
  char *myhost;
  struct utsname buffer;

  if (svc->config.service.host)
  {
    myhost = svc->config.service.host;
  }
  else
  {
    uname (&buffer);
    myhost = buffer.nodename;
  }

  svc->adminstate = UNLOCKED;
  svc->opstate = ENABLED;

  /* Wait for metadata and data to be available */

  if (!ping_client (svc->logger, "core-data", &svc->config.endpoints.data, svc->config.service.connectretries, svc->config.service.timeout, err))
  {
    return;
  }
  if (!ping_client (svc->logger, "core-metadata", &svc->config.endpoints.metadata, svc->config.service.connectretries, svc->config.service.timeout, err))
  {
    return;
  }

  *err = EDGEX_OK;

  /* Register device service in metadata */

  edgex_deviceservice *ds;
  ds = edgex_metadata_client_get_deviceservice
    (svc->logger, &svc->config.endpoints, svc->name, err);
  if (err->code)
  {
    iot_log_error (svc->logger, "get_deviceservice failed");
    return;
  }

  if (ds == NULL)
  {
    uint64_t millis = iot_time_msecs ();
    edgex_addressable *addr = edgex_metadata_client_get_addressable
      (svc->logger, &svc->config.endpoints, svc->name, err);
    if (err->code)
    {
      iot_log_error (svc->logger, "get_addressable failed");
      return;
    }
    if (addr == NULL)
    {
      addr = malloc (sizeof (edgex_addressable));
      memset (addr, 0, sizeof (edgex_addressable));
      addr->origin = millis;
      addr->name = strdup (svc->name);
      addr->method = strdup ("POST");
      addr->protocol = strdup ("HTTP");
      addr->address = strdup (myhost);
      addr->port = svc->config.service.port;
      addr->path = strdup (EDGEX_DEV_API_CALLBACK);

      addr->id = edgex_metadata_client_create_addressable
        (svc->logger, &svc->config.endpoints, addr, err);
      if (err->code)
      {
        iot_log_error (svc->logger, "create_addressable failed");
        return;
      }
    }

    ds = malloc (sizeof (edgex_deviceservice));
    memset (ds, 0, sizeof (edgex_deviceservice));
    ds->addressable = addr;
    ds->name = strdup (svc->name);
    ds->operatingState = ENABLED;
    ds->adminState = UNLOCKED;
    ds->created = millis;
    for (int n = 0; svc->config.service.labels[n]; n++)
    {
      edgex_strings *newlabel = malloc (sizeof (edgex_strings));
      newlabel->str = strdup (svc->config.service.labels[n]);
      newlabel->next = ds->labels;
      ds->labels = newlabel;
    }

    ds->id = edgex_metadata_client_create_deviceservice
      (svc->logger, &svc->config.endpoints, ds, err);
    if (err->code)
    {
      iot_log_error
        (svc->logger, "Unable to create device service in metadata");
      return;
    }
  }
  else
  {
    if (ds->addressable->port != svc->config.service.port || strcmp (ds->addressable->address, myhost))
    {
      iot_log_info (svc->logger, "Updating service endpoint in metadata");
      ds->addressable->port = svc->config.service.port;
      free (ds->addressable->address);
      ds->addressable->address = strdup (myhost);
      edgex_metadata_client_update_addressable (svc->logger, &svc->config.endpoints, ds->addressable, err);
      if (err->code)
      {
        iot_log_error (svc->logger, "update_addressable failed");
        return;
      }
    }
  }
  edgex_deviceservice_free (ds);

  /* Load DeviceProfiles from files and register in metadata */

  edgex_device_profiles_upload (svc, err);
  if (err->code)
  {
    return;
  }

  /* Obtain Devices from metadata */

  edgex_device *devs = edgex_metadata_client_get_devices
    (svc->logger, &svc->config.endpoints, svc->name, err);

  if (err->code)
  {
    iot_log_error (svc->logger, "Unable to retrieve device list from metadata");
    return;
  }

  edgex_devmap_populate_devices (svc->devices, devs);
  edgex_device_free (devs);

  /* Start REST server now so that we get the callbacks on device addition */

  svc->daemon = edgex_rest_server_create
    (svc->logger, svc->config.service.port, err);
  if (err->code)
  {
    return;
  }

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_CALLBACK, PUT | POST | DELETE, svc,
    edgex_device_handler_callback
  );

  /* Add Devices from configuration */

  if (config)
  {
    edgex_device_process_configured_devices
      (svc, toml_array_in (config, "DeviceList"), err);
    if (err->code)
    {
      return;
    }
  }

  /* Driver configuration */

  if (!svc->userfns.init (svc->userdata, svc->logger, svc->config.driverconf))
  {
    *err = EDGEX_DRIVER_UNSTART;
    iot_log_error (svc->logger, "Protocol driver initialization failed");
    return;
  }

  /* Get Provision Watchers */

  edgex_watcher *w = edgex_metadata_client_get_watchers (svc->logger, &svc->config.endpoints, svc->name, err);
  if (err->code)
  {
    iot_log_error (svc->logger, "Unable to retrieve provision watchers from metadata");
  }
  if (w)
  {
    iot_log_info
      (svc->logger, "Added %u provision watchers from metadata", edgex_watchlist_populate (svc->watchlist, w));
    edgex_watcher_free (w);
  }

  /* Start scheduled events */

  iot_scheduler_start (svc->scheduler);

  /* Register REST handlers */

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_DEVICE, GET | PUT | POST, svc,
    edgex_device_handler_device
  );

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_DISCOVERY, POST, svc,
    edgex_device_handler_discovery
  );

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_METRICS, GET, svc, edgex_device_handler_metrics
  );

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_CONFIG, GET, svc, edgex_device_handler_config
  );

  edgex_rest_server_register_handler
    (svc->daemon, EDGEX_DEV_API_VERSION, GET, svc, version_handler);

  edgex_rest_server_register_handler
  (
    svc->daemon, EDGEX_DEV_API_PING, GET, svc, ping_handler
  );

  /* Ready. Register ourselves and log that we have started. */

  if (svc->registry)
  {
    devsdk_registry_register_service
    (
      svc->registry,
      svc->name,
      myhost,
      svc->config.service.port,
      svc->config.service.checkinterval,
      err
    );
    if (err->code)
    {
      iot_log_error (svc->logger, "Unable to register service in registry");
      return;
    }
  }

  if (svc->config.service.startupmsg)
  {
    iot_log_info (svc->logger, svc->config.service.startupmsg);
  }
}

void devsdk_service_start (devsdk_service_t *svc, devsdk_error *err)
{
  toml_table_t *config = NULL;
  bool uploadConfig = false;
  devsdk_nvpairs *confpairs = NULL;

  svc->starttime = iot_time_msecs();

  iot_init ();
  iot_threadpool_start (svc->thpool);

  *err = EDGEX_OK;

  if (svc->regURL)
  {
    if (*svc->regURL == '\0')
    {
      config = edgex_device_loadConfig (svc->logger, svc->confdir, svc->profile, err);
      if (err->code)
      {
        return;
      }
      svc->regURL = edgex_device_getRegURL (config);
    }
    if (svc->regURL)
    {
      svc->registry = devsdk_registry_get_registry (svc->logger, svc->thpool, svc->regURL);
    }
    if (svc->registry == NULL)
    {
      iot_log_error (svc->logger, "Registry was requested but no location given");
      *err = EDGEX_INVALID_ARG;
      return;
    }
  }

  if (svc->registry)
  {
    // Wait for registry to be ready

    unsigned retries = 5;
    char *errc = getenv ("edgex_registry_retry_count");
    if (errc)
    {
      int rc = atoi (errc);
      if (rc > 0)
      {
        retries = rc;
      }
    }

    struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
    char *errw = getenv ("edgex_registry_retry_wait");
    if (errw)
    {
      int rw = atoi (errw);
      if (rw > 0)
      {
        delay.tv_sec = rw;
      }
    }

    while (!devsdk_registry_ping (svc->registry, err) && --retries)
    {
      nanosleep (&delay, NULL);
    }
    if (retries == 0)
    {
      iot_log_error (svc->logger, "registry service not running at %s", svc->regURL);
      *err = EDGEX_REMOTE_SERVER_DOWN;
      return;
    }

    iot_log_info (svc->logger, "Found registry service at %s", svc->regURL);
    svc->stopconfig = malloc (sizeof (atomic_bool));
    atomic_init (svc->stopconfig, false);
    confpairs = devsdk_registry_get_config
      (svc->registry, svc->name, svc->profile, edgex_device_updateConf, svc, svc->stopconfig, err);

    if (confpairs)
    {
      edgex_device_populateConfig (svc, confpairs, err);
      if (err->code)
      {
        devsdk_nvpairs_free (confpairs);
        return;
      }
    }
    else
    {
      iot_log_info (svc->logger, "Unable to get configuration from registry.");
      iot_log_info (svc->logger, "Will load from file.");
      uploadConfig = true;
      *err = EDGEX_OK;
    }
  }

  if (uploadConfig || (svc->registry == NULL))
  {
    if (config == NULL)
    {
      config = edgex_device_loadConfig (svc->logger, svc->confdir, svc->profile, err);
      if (err->code)
      {
        return;
      }
    }

    confpairs = edgex_device_parseToml (config);
    edgex_device_populateConfig (svc, confpairs, err);

    if (uploadConfig)
    {
      iot_log_info (svc->logger, "Uploading configuration to registry.");
      edgex_device_overrideConfig (svc->logger, svc->name, confpairs);
      devsdk_registry_put_config (svc->registry, svc->name, svc->profile, confpairs, err);
      if (err->code)
      {
        iot_log_error (svc->logger, "Unable to upload config: %s", err->reason);
        devsdk_nvpairs_free (confpairs);
        toml_free (config);
        return;
      }
    }
  }

  if (svc->config.logging.file)
  {
    free (svc->logger->to);
    svc->logger->to = strdup (svc->config.logging.file);
  }

  if (svc->registry)
  {
    devsdk_error e;
    devsdk_registry_query_service (svc->registry, "edgex-core-metadata", &svc->config.endpoints.metadata.host, &svc->config.endpoints.metadata.port, &e);
    devsdk_registry_query_service (svc->registry, "edgex-core-data", &svc->config.endpoints.data.host, &svc->config.endpoints.data.port, &e);
    devsdk_registry_query_service (svc->registry, "edgex-support-logging", &svc->config.endpoints.logging.host, &svc->config.endpoints.logging.port, &e);
  }
  else
  {
    edgex_device_parseTomlClients (svc->logger, toml_table_in (config, "Clients"), &svc->config.endpoints, err);
  }

  if (svc->config.logging.useremote)
  {
    if (ping_client (svc->logger, "support-logging", &svc->config.endpoints.logging, svc->config.service.connectretries, svc->config.service.timeout, err))
    {
      char url[URL_BUF_SIZE];
      snprintf
      (
        url, URL_BUF_SIZE - 1,
        "http://%s:%u/api/v1/logs",
        svc->config.endpoints.logging.host, svc->config.endpoints.logging.port
      );
      if (svc->config.logging.file)
      {
        svc->logger->next = iot_logger_alloc_custom (svc->name, svc->config.logging.level, url, edgex_log_torest, NULL, true);
      }
      else
      {
        svc->logger->impl = edgex_log_torest;
        free (svc->logger->to);
        svc->logger->to = strdup (url);
      }
    }
    else
    {
      devsdk_nvpairs_free (confpairs);
      toml_free (config);
      return;
    }
  }

  if (svc->config.device.profilesdir == NULL)
  {
    svc->config.device.profilesdir = strdup (svc->confdir);
  }

  iot_log_info (svc->logger, "Starting %s device service, version %s", svc->name, svc->version);
  iot_log_info (svc->logger, "EdgeX device SDK for C, version " CSDK_VERSION_STR);
  iot_log_debug (svc->logger, "Service configuration follows:");
  for (const devsdk_nvpairs *iter = confpairs; iter; iter = iter->next)
  {
    iot_log_debug (svc->logger, "%s=%s", iter->name, iter->value);
  }
  devsdk_nvpairs_free (confpairs);

  startConfigured (svc, config, err);

  toml_free (config);

  if (err->code == 0)
  {
    iot_log_info (svc->logger, "Service started in: %dms", iot_time_msecs() - svc->starttime);
    iot_log_info (svc->logger, "Listening on port: %d", svc->config.service.port);
  }
}

static void *doPost (void *p)
{
  postparams *pp = (postparams *) p;
  devsdk_error err = EDGEX_OK;
  edgex_data_client_add_event
  (
    pp->svc->logger,
    &pp->svc->config.endpoints,
    pp->event,
    &err
  );

  edgex_event_cooked_free (pp->event);
  free (pp);
  return NULL;
}

void devsdk_post_readings
(
  devsdk_service_t *svc,
  const char *devname,
  const char *resname,
  devsdk_commandresult *values
)
{
  edgex_device *dev = edgex_devmap_device_byname (svc->devices, devname);
  if (dev == NULL)
  {
    iot_log_error (svc->logger, "Post readings: no such device %s", devname);
    return;
  }

  const edgex_cmdinfo *command = edgex_deviceprofile_findcommand
    (resname, dev->profile, true);
  edgex_device_release (dev);

  if (command)
  {
    edgex_event_cooked *event = edgex_data_process_event
      (devname, command, values, svc->config.device.datatransform);

    if (event)
    {
      postparams *pp = malloc (sizeof (postparams));
      pp->svc = svc;
      pp->event = event;
      iot_threadpool_add_work (svc->thpool, doPost, pp, -1);
    }
  }
  else
  {
    iot_log_error (svc->logger, "Post readings: no such resource %s", resname);
  }
}

void devsdk_service_stop (devsdk_service_t *svc, bool force, devsdk_error *err)
{
  *err = EDGEX_OK;
  iot_log_debug (svc->logger, "Stop device service");
  if (svc->stopconfig)
  {
    *svc->stopconfig = true;
  }
  if (svc->scheduler)
  {
    iot_scheduler_stop (svc->scheduler);
  }
  if (svc->daemon)
  {
    edgex_rest_server_destroy (svc->daemon);
  }
  svc->userfns.stop (svc->userdata, force);
  edgex_devmap_clear (svc->devices);
  if (svc->registry)
  {
    devsdk_registry_deregister_service (svc->registry, svc->name, err);
    if (err->code)
    {
      iot_log_error (svc->logger, "Unable to deregister service from registry");
    }
  }
  iot_scheduler_free (svc->scheduler);
  iot_threadpool_wait (svc->thpool);
  iot_log_info (svc->logger, "Stopped device service");
}

void devsdk_service_free (devsdk_service_t *svc)
{
  if (svc)
  {
    edgex_devmap_free (svc->devices);
    edgex_watchlist_free (svc->watchlist);
    iot_threadpool_free (svc->thpool);
    devsdk_registry_free (svc->registry);
    devsdk_registry_fini ();
    pthread_mutex_destroy (&svc->discolock);
    iot_logger_free (svc->logger);
    edgex_device_freeConfig (svc);
    free (svc->stopconfig);
    free (svc);
    iot_fini ();
  }
}
