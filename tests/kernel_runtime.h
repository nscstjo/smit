/* Deterministic protocol harness only; does not simulate kernel locking/USB URBs. */
#ifndef SMIT_TEST_RUNTIME_H
#define SMIT_TEST_RUNTIME_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct device {
    int unused;
};
struct usb_device {
    struct device dev;
};
struct mutex {
    int unused;
};
struct work_struct {
    void (*fn)(struct work_struct *);
};
struct delayed_work {
    struct work_struct work;
};
struct workqueue_struct {
    int unused;
};
struct completion {
    bool done;
};
static unsigned long jiffies;
#define module_param(a, b, c)
#define MODULE_PARM_DESC(a, b)
#define KERN_DEBUG ""
#define DUMP_PREFIX_NONE 0
#define WQ_MEM_RECLAIM 0
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) (x)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define lockdep_assert_held(x) ((void)(x))
#define dev_warn(d, ...) ((void)(d))
#define dev_info(d, ...)                                                                           \
    do {                                                                                           \
        (void)(d);                                                                                 \
        if (0)                                                                                     \
            printf(__VA_ARGS__);                                                                   \
    } while (0)
#define dev_dbg(d, ...) ((void)(d))
#define dev_info_ratelimited(d, ...) ((void)(d))
#define print_hex_dump(...) ((void)0)
static void mutex_init(struct mutex *m)
{
    (void)m;
}
static void mutex_lock(struct mutex *m)
{
    (void)m;
}
static void mutex_unlock(struct mutex *m)
{
    (void)m;
}
static void msleep(unsigned int n)
{
    jiffies += n;
}
static void init_completion(struct completion *c)
{
    c->done = false;
}
static void complete(struct completion *c)
{
    c->done = true;
}
static int wait_for_completion_interruptible(struct completion *c)
{
    assert(c->done);
    return 0;
}
static int mutex_lock_interruptible(struct mutex *m)
{
    (void)m;
    return 0;
}
static void cancel_work_sync(struct work_struct *w)
{
    (void)w;
}
static void flush_work(struct work_struct *w)
{
    (void)w;
}
#define INIT_WORK(w, f) ((w)->fn = (f))
#define INIT_DELAYED_WORK(w, f) INIT_WORK(&(w)->work, f)
static struct workqueue_struct *alloc_ordered_workqueue(const char *n, int flags)
{
    (void)n;
    (void)flags;
    return calloc(1, sizeof(struct workqueue_struct));
}
static void destroy_workqueue(struct workqueue_struct *q)
{
    free(q);
}
static void flush_workqueue(struct workqueue_struct *q)
{
    (void)q;
}
static void queue_work(struct workqueue_struct *q, struct work_struct *w)
{
    (void)q;
    w->fn(w);
}
static void queue_delayed_work(struct workqueue_struct *q, struct delayed_work *w,
                               unsigned long delay)
{
    (void)q;
    (void)w;
    (void)delay;
}
static void cancel_delayed_work_sync(struct delayed_work *w)
{
    (void)w;
}
static unsigned int usb_sndbulkpipe(struct usb_device *d, unsigned int ep)
{
    (void)d;
    return ep;
}
static unsigned int usb_rcvbulkpipe(struct usb_device *d, unsigned int ep)
{
    (void)d;
    return ep;
}
static int usb_bulk_msg(struct usb_device *, unsigned int, void *, int, int *, int);
#define USB_DIR_OUT 0
#define USB_TYPE_VENDOR 0x40
#define USB_RECIP_DEVICE 0
static unsigned int usb_sndctrlpipe(struct usb_device *d, unsigned int ep)
{
    (void)d;
    return ep;
}
static int usb_control_msg(struct usb_device *, unsigned int, u8, u8, unsigned short,
                           unsigned short, void *, unsigned short, int);
#endif
