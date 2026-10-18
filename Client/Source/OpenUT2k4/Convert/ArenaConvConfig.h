#pragma once

/**
 * 1 when the converter sources (export kinds, maps, driver) are complete enough to be compiled into the
 * module. Kept at 0 while a part is being written so the shared module always builds.
 */
#define ARENA_CONV_BUILD 1
