/* Copyright (c) Mark Harmstone 2016-17
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#include "btrfs_drv.h"

static void do_calc(device_extension* Vcb, calc_job* cj, uint8_t* src, uint32_t* dest) {
    // FIXME - do at DISPATCH irql

    *dest = ~calc_crc32c(0xffffffff, src, Vcb->superblock.sector_size);

    if (InterlockedDecrement(&cj->left) == 0)
        KeSetEvent(&cj->event, 0, false);
}

void do_calc_job(device_extension* Vcb, uint8_t* data, uint32_t sectors, uint32_t* csum) {
    KIRQL irql;
    calc_job cj;

    cj.data = data;
    cj.csum = csum;
    cj.left = cj.not_started = sectors;
    KeInitializeEvent(&cj.event, NotificationEvent, false);

    KeAcquireSpinLock(&Vcb->calcthreads.spinlock, &irql);

    InsertTailList(&Vcb->calcthreads.job_list, &cj.list_entry);

    KeSetEvent(&Vcb->calcthreads.event, 0, false);
    KeClearEvent(&Vcb->calcthreads.event);

    KeReleaseSpinLock(&Vcb->calcthreads.spinlock, irql);

    while (true) {
        KIRQL irql;
        uint8_t* src;
        uint32_t* dest;
        bool last_one = false;

        KeAcquireSpinLock(&Vcb->calcthreads.spinlock, &irql);

        if (cj.not_started == 0) {
            KeReleaseSpinLock(&Vcb->calcthreads.spinlock, irql);
            break;
        }

        src = cj.data;
        cj.data += Vcb->superblock.sector_size;

        dest = cj.csum;
        cj.csum++;

        cj.not_started--;
        if (cj.not_started == 0) {
            RemoveEntryList(&cj.list_entry);
            last_one = true;
        }

        KeReleaseSpinLock(&Vcb->calcthreads.spinlock, irql);

        do_calc(Vcb, &cj, src, dest);

        if (last_one)
            break;
    }

    KeWaitForSingleObject(&cj.event, Executive, KernelMode, false, NULL);
}

_Function_class_(KSTART_ROUTINE)
void __stdcall calc_thread(void* context) {
    drv_calc_thread* thread = context;
    device_extension* Vcb = thread->DeviceObject->DeviceExtension;

    ObReferenceObject(thread->DeviceObject);

    KeSetSystemAffinityThread(1 << thread->number);

    while (true) {
        KeWaitForSingleObject(&Vcb->calcthreads.event, Executive, KernelMode, false, NULL);

        while (true) {
            KIRQL irql;
            calc_job* cj;
            uint8_t* src;
            uint32_t* dest;
            bool last_one = false;

            KeAcquireSpinLock(&Vcb->calcthreads.spinlock, &irql);

            if (IsListEmpty(&Vcb->calcthreads.job_list)) {
                KeReleaseSpinLock(&Vcb->calcthreads.spinlock, irql);
                break;
            }

            cj = CONTAINING_RECORD(Vcb->calcthreads.job_list.Flink, calc_job, list_entry);

            src = cj->data;
            cj->data += Vcb->superblock.sector_size;

            dest = cj->csum;
            cj->csum++;

            cj->not_started--;

            if (cj->not_started == 0) {
                RemoveEntryList(&cj->list_entry);
                last_one = true;
            }

            KeReleaseSpinLock(&Vcb->calcthreads.spinlock, irql);

            do_calc(Vcb, cj, src, dest);

            if (last_one)
                break;
        }

        if (thread->quit)
            break;
    }

    ObDereferenceObject(thread->DeviceObject);

    KeSetEvent(&thread->finished, 0, false);

    PsTerminateSystemThread(STATUS_SUCCESS);
}
