/*
 * Copyright (c) 2020
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <errno.h>
#include "devsdk/devsdk-base.h"
#include "devutil.h"

void devsdk_strings_free (devsdk_strings *strs)
{
  while (strs)
  {
    devsdk_strings *current = strs;
    free (strs->str);
    strs = strs->next;
    free (current);
  }
}

devsdk_nvpairs *devsdk_nvpairs_new (const char *name, const char *value, devsdk_nvpairs *list)
{
  devsdk_nvpairs *result = malloc (sizeof (devsdk_nvpairs));
  result->name = strdup (name);
  result->value = strdup (value);
  result->next = list;
  return result;
}

const char *devsdk_nvpairs_value (const devsdk_nvpairs *nvp, const char *name)
{
  if (name)
  {
    for (; nvp; nvp = nvp->next)
    {
      if (strcmp (nvp->name, name) == 0)
      {
        return nvp->value;
      }
    }
  }
  return NULL;
}

bool devsdk_nvpairs_long_value (const devsdk_nvpairs *nvp, const char *name, long *val)
{
  bool result = false;
  const char *v = devsdk_nvpairs_value (nvp, name);
  if (v && *v)
  {
    char *end = NULL;
    errno = 0;
    long l = strtol (v, &end, 0);
    if (errno == 0 && *end == 0)
    {
      *val = l;
      result = true;
    }
  }
  return result;
}

bool devsdk_nvpairs_ulong_value (const devsdk_nvpairs *nvp, const char *name, unsigned long *val)
{
  bool result = false;
  const char *v = devsdk_nvpairs_value (nvp, name);
  if (v && *v)
  {
    char *end = NULL;
    errno = 0;
    unsigned long l = strtoul (v, &end, 0);
    if (errno == 0 && *end == 0)
    {
      *val = l;
      result = true;
    }
  }
  return result;
}

bool devsdk_nvpairs_float_value (const devsdk_nvpairs *nvp, const char *name, float *val)
{
  bool result = false;
  const char *v = devsdk_nvpairs_value (nvp, name);
  if (v && *v)
  {
    char *end = NULL;
    errno = 0;
    float f = strtof (v, &end);
    if (errno == 0 && *end == 0)
    {
      *val = f;
      result = true;
    }
  }
  return result;
}

devsdk_nvpairs *devsdk_nvpairs_dup (const devsdk_nvpairs *p)
{
  devsdk_nvpairs *result = NULL;
  devsdk_nvpairs *copy;
  devsdk_nvpairs **last = &result;
  while (p)
  {
    copy = malloc (sizeof (devsdk_nvpairs));
    copy->name = strdup (p->name);
    copy->value = strdup (p->value);
    copy->next = NULL;
    *last = copy;
    last = &(copy->next);
    p = p->next;
  }
  return result;
}

void devsdk_nvpairs_free (devsdk_nvpairs *p)
{
  while (p)
  {
    devsdk_nvpairs *current = p;
    free (p->name);
    free (p->value);
    p = p->next;
    free (current);
  }
}

struct devsdk_protocols
{
  char *name;
  devsdk_nvpairs *properties;
  struct devsdk_protocols *next;
};

devsdk_protocols *devsdk_protocols_new (const char *name, const devsdk_nvpairs *properties, devsdk_protocols *list)
{
  devsdk_protocols *result = malloc (sizeof (devsdk_protocols));
  result->name = strdup (name);
  result->properties = devsdk_nvpairs_dup (properties);
  result->next = list;
  return result;
}

const devsdk_nvpairs *devsdk_protocols_properties (const devsdk_protocols *prots, const char *name)
{
  if (name)
  {
    for (; prots; prots = prots->next)
    {
      if (strcmp (prots->name, name) == 0)
      {
        return prots->properties;
      }
    }
  }
  return NULL;
}

devsdk_protocols *devsdk_protocols_dup (const devsdk_protocols *e)
{
  devsdk_protocols *result = NULL;
  for (const devsdk_protocols *p = e; p; p = p->next)
  {
    devsdk_protocols *newprot = malloc (sizeof (devsdk_protocols));
    newprot->name = strdup (p->name);
    newprot->properties = devsdk_nvpairs_dup (p->properties);
    newprot->next = result;
    result = newprot;
  }
  return result;
}

void devsdk_protocols_free (devsdk_protocols *e)
{
  if (e)
  {
    free (e->name);
    devsdk_nvpairs_free (e->properties);
    devsdk_protocols_free (e->next);
    free (e);
  }
}

/* Macro for generating single-linked-list comparison functions.
 * Assumes a "next" pointer and that the key (name) field is a string.
 */

#define LIST_EQUAL_FUNCTION(TYPENAME,NAMEFIELD,CMPFUNC)           \
bool TYPENAME ## _equal (const TYPENAME *l1, const TYPENAME *l2)  \
{                                                                 \
  const TYPENAME *l;                                              \
  const TYPENAME *found;                                          \
  unsigned n1 = 0;                                                \
  unsigned n2 = 0;                                                \
  for (l = l1; l; l = l->next, n1++);                             \
  for (l = l2; l; l = l->next, n2++);                             \
  if (n1 != n2) return false;                                     \
  for (l = l1; l; l = l->next)                                    \
  {                                                               \
    for (found = l2; found; found = found->next)                  \
    {                                                             \
      if (strcmp (l->NAMEFIELD, found->NAMEFIELD) == 0) break;    \
    }                                                             \
    if (!found || !CMPFUNC (l, found)) return false;              \
  }                                                               \
  return true;                                                    \
}

static bool pair_equal (const devsdk_nvpairs *p1, const devsdk_nvpairs *p2)
{
  return (strcmp (p1->value, p2->value) == 0);
}

static LIST_EQUAL_FUNCTION(devsdk_nvpairs, name, pair_equal)

static bool protocol_equal (const devsdk_protocols *p1, const devsdk_protocols *p2)
{
  return devsdk_nvpairs_equal (p1->properties, p2->properties);
}

LIST_EQUAL_FUNCTION(devsdk_protocols, name, protocol_equal)

static bool autoevent_equal (const edgex_device_autoevents *e1, const edgex_device_autoevents *e2)
{
  return strcmp (e1->frequency, e2->frequency) == 0 && e1->onChange == e2->onChange;
}

LIST_EQUAL_FUNCTION(edgex_device_autoevents, resource, autoevent_equal)
