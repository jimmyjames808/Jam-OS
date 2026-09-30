/* libos's driver support: what driver_user.c (<jam/driver.h>'s
 * implementation) and driver_crt.c (a driver process's main) share. */
#pragma once

#include <jam/driver.h>

/* The running driver's start: set by driver_crt.c's main before it calls
 * driver_main; NULL in a program that isn't a driver (drv_report then
 * adds no name, drv_vmo_create finds no DR_DMA). */
extern const struct driver_start *libos_driver_start;
