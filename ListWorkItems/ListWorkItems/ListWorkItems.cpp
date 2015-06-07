// Copyright (c) 2015, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

#include "stdafx.h"
#include "exclusivity.h"

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

static const auto ParentNode_Offset = 0x640;

////////////////////////////////////////////////////////////////////////////////
//
// types
//

struct _EX_WORK_QUEUE {
  //+0x000 WorkPriQueue     : _KPRIQUEUE
  _KPRIQUEUE WorkPriQueue;
};

struct Win81 {
  struct _ENODE {
    //+0x000 Ncb              : _KNODE
    //+0x0c0 ExWorkQueues     : [8] Ptr64 _EX_WORK_QUEUE
    //+0x100 ExWorkQueue      : _EX_WORK_QUEUE
    BYTE Unused[0x100];
    _EX_WORK_QUEUE ExWorkQueue;
  };
};

struct Win10 {
  struct _ENODE {
    //+0x000 Ncb              : _KNODE
    //+0x100 ExWorkQueues     : [8] Ptr64 _EX_WORK_QUEUE
    //+0x140 ExWorkQueue      : _EX_WORK_QUEUE
    BYTE Unused[0x140];
    _EX_WORK_QUEUE ExWorkQueue;
  };
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

static WORK_QUEUE_ITEM *g_WorkItems[50] = {};
static long g_NumbeOfWorkItemsExecuted = 0;

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

template <typename Version> static void ListWorkItems() {
  const auto exNode0 = *reinterpret_cast<Version::_ENODE **>(
                           reinterpret_cast<BYTE *>(KeGetPcr()->CurrentPrcb) +
                           ParentNode_Offset);

  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Node %p\n", exNode0);

  // Enumerate all workitems from all lists
  for (auto priority = 0;
       priority <
       RTL_NUMBER_OF(exNode0->ExWorkQueue.WorkPriQueue.EntryListHead);
       ++priority) {
    auto &list = exNode0->ExWorkQueue.WorkPriQueue.EntryListHead[priority];
    auto next = list.Flink;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Priority %d\n",
               priority);

    int index = 0;
    while (next != &list) {
      auto item = CONTAINING_RECORD(next, WORK_QUEUE_ITEM, List);
      DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                 "  %-3d ExWorkItem (%p) Routine (%p) Parameter (%p)\n", index,
                 item, item->WorkerRoutine, item->Parameter);
      next = next->Flink;
      index++;
    }
  }
}

EXTERN_C static void DriverUnload(_In_ PDRIVER_OBJECT DriverObject) {
  PAGED_CODE();
  UNREFERENCED_PARAMETER(DriverObject);

  DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Executed Items %d\n",
             g_NumbeOfWorkItemsExecuted);
  if (g_NumbeOfWorkItemsExecuted == RTL_NUMBER_OF(g_WorkItems)) {
    for (auto item : g_WorkItems) {
      ExFreePoolWithTag(item, LIST_POOL_TAG_NAME);
    }
  }
}

EXTERN_C NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
                              _In_ PUNICODE_STRING RegistryPath) {
  PAGED_CODE();
  UNREFERENCED_PARAMETER(RegistryPath);

  DriverObject->DriverUnload = DriverUnload;
  DBG_BREAK();

  RTL_OSVERSIONINFOW version = {sizeof(version)};
  auto status = RtlGetVersion(&version);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  // Only supports 8.1 and 10
  bool isWindows10 = false;
  if (version.dwMajorVersion == 6 && version.dwMinorVersion == 3) {
  } else if (version.dwMajorVersion == 10 && version.dwMinorVersion == 0) {
    isWindows10 = true;
  } else {
    return STATUS_UNSUCCESSFUL;
  }

  // Allocate test work items
  for (int i = 0; i < RTL_NUMBER_OF(g_WorkItems); i++) {
    auto item = reinterpret_cast<WORK_QUEUE_ITEM *>(ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(WORK_QUEUE_ITEM), LIST_POOL_TAG_NAME));
    if (!item) {
      for (auto item : g_WorkItems) {
        if (item) {
          ExFreePoolWithTag(item, LIST_POOL_TAG_NAME);
        }
      }
      return STATUS_UNSUCCESSFUL;
    }
    ExInitializeWorkItem(item, [](void *p) {
      auto x = InterlockedIncrement(reinterpret_cast<long *>(p));
      DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                 ">> WorkItem executed %d\n", x);
    }, &g_NumbeOfWorkItemsExecuted);
    g_WorkItems[i] = item;
  }

  // Lock all other processors
  auto exclusivity = ExclGainExclusivity();
  if (!exclusivity) {
    for (auto item : g_WorkItems) {
      ExFreePoolWithTag(item, LIST_POOL_TAG_NAME);
    }
    return STATUS_UNSUCCESSFUL;
  }

  // Lock this processor
  auto oldIrql = KeRaiseIrqlToDpcLevel();
  for (auto item : g_WorkItems) {
    ExQueueWorkItem(item, DelayedWorkQueue);
  }

  DBG_BREAK();
  if (isWindows10) {
    ListWorkItems<Win10>();
  } else {
    ListWorkItems<Win81>();
  }

  KeLowerIrql(oldIrql);
  ExclReleaseExclusivity(exclusivity);
  return STATUS_SUCCESS;
}
