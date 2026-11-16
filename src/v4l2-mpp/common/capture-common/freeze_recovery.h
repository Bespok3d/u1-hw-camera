#ifndef FREEZE_RECOVERY_H
#define FREEZE_RECOVERY_H

#include <stdbool.h>
#include <time.h>

/* On firmware 2.1.0 the printer's video service can stop getting pictures from the camera and not
 * notice by itself: the stream stays black until something asks the service for a photo. A photo
 * that fails makes the service rebuild its camera pipeline, and the pictures come back; that is why
 * opening the Snapmaker app brings the camera back.
 *
 * So when the tap answers but its camera channel stays silent, the capture asks for a photo itself.
 * It waits first, so the gap while the service starts its camera is never taken for a freeze. It
 * asks at most once a minute, because a rebuild drops the camera for a moment, and it gives up on a
 * freeze that a few photos did not end, so a camera no photo can wake is not rebuilt forever. */
#define FREEZE_RECOVERY_SILENCE_S 10
#define FREEZE_RECOVERY_REPEAT_S 60
#define FREEZE_RECOVERY_PHOTOS_PER_FREEZE 5

typedef struct {
    bool camera_silent;
    time_t silent_since;
    int photos_this_freeze;
    bool photo_asked;
    time_t photo_asked_at;
} freeze_recovery_t;

/* A picture ends the freeze. The last photo's time is kept, so a camera that comes back and freezes
 * again at once is still not asked more than once a minute. */
static inline freeze_recovery_t freeze_recovery_picture_arrived(freeze_recovery_t recovery)
{
    return (freeze_recovery_t){
        .photo_asked = recovery.photo_asked,
        .photo_asked_at = recovery.photo_asked_at,
    };
}

/* Not a picture and not silence, such as the service restarting its camera for the rebuild a photo
 * caused: the silence is timed afresh, but the freeze, and the photos it has had, go on. */
static inline freeze_recovery_t freeze_recovery_answer_without_picture(freeze_recovery_t recovery)
{
    recovery.camera_silent = false;
    return recovery;
}

static inline freeze_recovery_t freeze_recovery_camera_silent(freeze_recovery_t recovery,
                                                              time_t now)
{
    if (recovery.camera_silent)
        return recovery;
    recovery.camera_silent = true;
    recovery.silent_since = now;
    return recovery;
}

static inline bool freeze_recovery_photo_is_due(freeze_recovery_t recovery, time_t now)
{
    if (!recovery.camera_silent || now - recovery.silent_since < FREEZE_RECOVERY_SILENCE_S)
        return false;
    if (recovery.photos_this_freeze >= FREEZE_RECOVERY_PHOTOS_PER_FREEZE)
        return false;
    return !recovery.photo_asked || now - recovery.photo_asked_at >= FREEZE_RECOVERY_REPEAT_S;
}

static inline freeze_recovery_t freeze_recovery_photo_asked(freeze_recovery_t recovery,
                                                            time_t now)
{
    recovery.photos_this_freeze += 1;
    recovery.photo_asked = true;
    recovery.photo_asked_at = now;
    return recovery;
}

#endif /* FREEZE_RECOVERY_H */
