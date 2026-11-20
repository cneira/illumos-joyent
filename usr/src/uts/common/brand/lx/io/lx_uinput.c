/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License (CDDL), version 1.0.
 * A full copy of the CDDL should have accompanied this source.
 *
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Small, zone-scoped uinput/evdev device for LX gamepads.  Linux ioctl
 * numbers and payloads are kept explicit so this driver has no dependency
 * on Linux headers from the build machine.
 */
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/poll.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/zone.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/stat.h>
#include <sys/id_space.h>
#include <sys/model.h>
#include <sys/vnode.h>
#include <sys/pathname.h>

#define	LUI_PRODUCER 0
#define	LUI_EVENT_BASE 1
#define	LUI_MAX_DEVICES 4
#define	LUI_CLONE_BASE (LUI_EVENT_BASE + LUI_MAX_DEVICES)
#define	LUI_READER_BASE 65536
#define	LUI_RING 256
#define	LUI_MAX_READERS 64
#define	LUI_EV_MAX 31
#define	LUI_KEY_MAX 767
#define	LUI_ABS_MAX 63
#define	LUI_FF_MAX 127
#define	LUI_BTN_SOUTH 304

#define	LUI_IOC_TYPE(c) (((uint32_t)(c) >> 8) & 0xff)
#define	LUI_IOC_NR(c) ((uint32_t)(c) & 0xff)
#define	LUI_IOC_SIZE(c) (((uint32_t)(c) >> 16) & 0x3fff)

typedef struct lui_id {
	uint16_t bustype, vendor, product, version;
} lui_id_t;

typedef struct lui_setup {
	lui_id_t id;
	char name[80];
	uint32_t ff_effects_max;
} lui_setup_t;

typedef struct lui_absinfo {
	int32_t value, minimum, maximum, fuzz, flat, resolution;
} lui_absinfo_t;

typedef struct lui_abs_setup {
	uint16_t code;
	uint16_t pad;
	lui_absinfo_t absinfo;
} lui_abs_setup_t;

typedef struct lui_event {
	int64_t sec, usec;
	uint16_t type, code;
	int32_t value;
} lui_event_t;

typedef struct lui_event32 {
	int32_t sec, usec;
	uint16_t type, code;
	int32_t value;
} lui_event32_t;

typedef struct lui_state {
	struct lui_state *next;
	struct lui_reader *reader_head;
	zoneid_t zone;
	minor_t producer_minor;
	uint_t event_index;
	boolean_t producer, created;
	uint_t readers, active;
	kmutex_t lock;
	kcondvar_t cv;
	lui_setup_t setup;
	lui_absinfo_t axes[LUI_ABS_MAX + 1];
	uint8_t ev[(LUI_EV_MAX + 8) / 8];
	uint8_t key[(LUI_KEY_MAX + 8) / 8];
	uint8_t key_state[(LUI_KEY_MAX + 8) / 8];
	uint8_t abs[(LUI_ABS_MAX + 8) / 8];
	uint8_t ff[(LUI_FF_MAX + 8) / 8];
} lui_state_t;

typedef struct lui_reader {
	struct lui_reader *next;
	struct lui_reader *state_next;
	lui_state_t *state;
	zoneid_t zone;
	minor_t minor;
	uint_t head, tail, count;
	uint_t active;
	boolean_t closing;
	struct pollhead pollhead;
	lui_event_t events[LUI_RING];
} lui_reader_t;

static dev_info_t *lui_dip;
static kmutex_t lui_list_lock;
static lui_state_t *lui_states;
static lui_reader_t *lui_readers;
static id_space_t *lui_producer_ids;
static id_space_t *lui_reader_ids;

static lui_state_t *
lui_find_event(zoneid_t zid, uint_t index)
{
	lui_state_t *s;
	for (s = lui_states; s != NULL; s = s->next) {
		if (s->zone == zid && s->event_index == index)
			return (s);
	}
	return (NULL);
}

static lui_state_t *
lui_find_producer(zoneid_t zid, minor_t minor)
{
	lui_state_t *s;
	for (s = lui_states; s != NULL; s = s->next) {
		if (s->zone == zid && s->producer_minor == minor &&
		    s->producer)
			return (s);
	}
	return (NULL);
}

static lui_reader_t *
lui_find_reader(minor_t minor, zoneid_t zid)
{
	lui_reader_t *reader;
	for (reader = lui_readers; reader != NULL; reader = reader->next) {
		if (reader->minor == minor && reader->zone == zid)
			return (reader);
	}
	return (NULL);
}

static void
lui_free(lui_state_t *s)
{
	lui_state_t **sp;
	for (sp = &lui_states; *sp != s; sp = &(*sp)->next)
		ASSERT(*sp != NULL);
	*sp = s->next;
	id_free(lui_producer_ids, s->producer_minor);
	cv_destroy(&s->cv);
	mutex_destroy(&s->lock);
	kmem_free(s, sizeof (*s));
}

/*
 * pollwakeup() can acquire a pollcache lock held by a thread in lui_poll().
 * Never call it while holding either driver lock.  Keep each reader alive
 * across the unlocked wakeup; close defers its free while active is nonzero.
 */
static uint_t
lui_collect_wakers(lui_state_t *s, lui_reader_t **wakers)
{
	lui_reader_t *reader;
	uint_t n = 0;

	ASSERT(MUTEX_HELD(&s->lock));
	for (reader = s->reader_head; reader != NULL;
	    reader = reader->state_next) {
		ASSERT(n < LUI_MAX_READERS);
		reader->active++;
		wakers[n++] = reader;
	}
	return (n);
}

/*
 * The pollhead is embedded in the reader, so every polldat still linked on
 * it must be detached before the memory is freed.  Otherwise a later
 * pollwakeup() on a recycled reader walks a stale ph_list into a freed
 * pollcache.  Like pollwakeup(), this takes poll locks: no driver lock may
 * be held.
 */
static void
lui_reader_free(lui_reader_t *reader)
{
	if (reader->pollhead.ph_list != NULL) {
		pollwakeup(&reader->pollhead, POLLERR);
		pollhead_clean(&reader->pollhead);
	}
	kmem_free(reader, sizeof (*reader));
}

static void
lui_wake_readers(lui_state_t *s, lui_reader_t **wakers, uint_t n,
    short events)
{
	uint_t i, ndead = 0;

	for (i = 0; i < n; i++)
		pollwakeup(&wakers[i]->pollhead, events);
	mutex_enter(&s->lock);
	for (i = 0; i < n; i++) {
		ASSERT(wakers[i]->active != 0);
		wakers[i]->active--;
		/* Compact in place; ndead <= i. */
		if (wakers[i]->closing && wakers[i]->active == 0)
			wakers[ndead++] = wakers[i];
	}
	mutex_exit(&s->lock);
	for (i = 0; i < ndead; i++)
		lui_reader_free(wakers[i]);
}

/* Each producer gets a distinct clone minor and event slot in its zone. */
static int
lui_open(dev_t *devp, int flags, int otyp, cred_t *cr)
{
	lui_state_t *s;
	lui_state_t *new_state = NULL;
	lui_reader_t *new_reader = NULL;
	minor_t minor = getminor(*devp);
	zoneid_t zid = crgetzoneid(cr);
	uint_t index;
	int error = 0;
	(void) flags;
	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (minor == LUI_PRODUCER) {
		new_state = kmem_zalloc(sizeof (*new_state), KM_SLEEP);
		new_state->zone = zid;
		new_state->producer = B_TRUE;
		new_state->ev[0] = 1; /* EV_SYN is implicit in Linux uinput. */
		mutex_init(&new_state->lock, NULL, MUTEX_DEFAULT, NULL);
		cv_init(&new_state->cv, NULL, CV_DEFAULT, NULL);
	} else if (minor >= LUI_EVENT_BASE && minor < LUI_CLONE_BASE) {
		new_reader = kmem_zalloc(sizeof (*new_reader), KM_SLEEP);
	}
	mutex_enter(&lui_list_lock);
	if (minor == LUI_PRODUCER) {
		for (index = 0; index < LUI_MAX_DEVICES; index++) {
			if (lui_find_event(zid, index) == NULL)
				break;
		}
		if (index == LUI_MAX_DEVICES)
			error = ENOSPC;
		else if ((new_state->producer_minor =
		    id_allocff_nosleep(lui_producer_ids)) == (minor_t)-1)
			error = ENOSPC;
		else {
			new_state->event_index = index;
			new_state->next = lui_states;
			lui_states = new_state;
			*devp = makedevice(getmajor(*devp),
			    new_state->producer_minor);
			new_state = NULL;
		}
	} else if (minor >= LUI_EVENT_BASE && minor < LUI_CLONE_BASE) {
		s = lui_find_event(zid, minor - LUI_EVENT_BASE);
		if (s == NULL)
			error = ENODEV;
		else {
			mutex_enter(&s->lock);
			if (!s->created)
				error = ENODEV;
			else if (s->readers >= LUI_MAX_READERS)
				error = EMFILE;
			else if ((new_reader->minor =
			    id_allocff_nosleep(lui_reader_ids)) == (minor_t)-1)
				error = ENOSPC;
			else {
				s->readers++;
				new_reader->zone = zid;
				new_reader->state = s;
				new_reader->next = lui_readers;
				lui_readers = new_reader;
				new_reader->state_next = s->reader_head;
				s->reader_head = new_reader;
				*devp = makedevice(getmajor(*devp),
				    new_reader->minor);
				new_reader = NULL;
			}
			mutex_exit(&s->lock);
		}
	} else {
		error = ENXIO;
	}
	mutex_exit(&lui_list_lock);
	if (new_state != NULL) {
		cv_destroy(&new_state->cv);
		mutex_destroy(&new_state->lock);
		kmem_free(new_state, sizeof (*new_state));
	}
	if (new_reader != NULL)
		kmem_free(new_reader, sizeof (*new_reader));
	return (error);
}

static int
lui_close(dev_t dev, int flags, int otyp, cred_t *cr)
{
	lui_state_t *s;
	lui_reader_t *reader = NULL;
	lui_reader_t *wakers[LUI_MAX_READERS];
	lui_reader_t **rp;
	lui_reader_t **srp;
	uint_t nwake = 0;
	minor_t minor = getminor(dev);
	(void) flags;
	(void) otyp;
	mutex_enter(&lui_list_lock);
	if (minor >= LUI_READER_BASE) {
		reader = lui_find_reader(minor, crgetzoneid(cr));
		s = reader != NULL ? reader->state : NULL;
	} else {
		s = lui_find_producer(crgetzoneid(cr), minor);
	}
	if (s == NULL) {
		mutex_exit(&lui_list_lock);
		return (ENODEV);
	}
	mutex_enter(&s->lock);
	if (reader != NULL) {
		ASSERT(s->readers != 0);
		s->readers--;
		for (rp = &lui_readers; *rp != reader; rp = &(*rp)->next)
			ASSERT(*rp != NULL);
		*rp = reader->next;
		for (srp = &s->reader_head; *srp != reader;
		    srp = &(*srp)->state_next)
			ASSERT(*srp != NULL);
		*srp = reader->state_next;
		id_free(lui_reader_ids, reader->minor);
		if (reader->active != 0) {
			reader->closing = B_TRUE;
			reader = NULL;
		}
	} else if (minor == s->producer_minor) {
		s->producer = B_FALSE;
		s->created = B_FALSE;
		cv_broadcast(&s->cv);
		nwake = lui_collect_wakers(s, wakers);
		if (nwake != 0)
			s->active++;
	}
	mutex_exit(&s->lock);
	if (!s->producer && s->readers == 0 && s->active == 0)
		lui_free(s);
	mutex_exit(&lui_list_lock);
	if (reader != NULL)
		lui_reader_free(reader);
	if (nwake != 0) {
		lui_wake_readers(s, wakers, nwake, POLLHUP);
		mutex_enter(&lui_list_lock);
		ASSERT(s->active != 0);
		s->active--;
		if (!s->producer && s->readers == 0 && s->active == 0)
			lui_free(s);
		mutex_exit(&lui_list_lock);
	}
	return (0);
}

static int
lui_read(dev_t dev, struct uio *uio, cred_t *cr)
{
	lui_state_t *s;
	lui_reader_t *reader;
	lui_event_t event;
	lui_event32_t event32;
	boolean_t compat = get_udatamodel() != DATAMODEL_NATIVE;
	size_t event_size = compat ? sizeof (event32) : sizeof (event);
	int error = 0;
	if (getminor(dev) < LUI_READER_BASE || uio->uio_resid < event_size)
		return (EINVAL);
	mutex_enter(&lui_list_lock);
	reader = lui_find_reader(getminor(dev), crgetzoneid(cr));
	if (reader == NULL) {
		mutex_exit(&lui_list_lock);
		return (ENODEV);
	}
	s = reader->state;
	mutex_enter(&s->lock);
	mutex_exit(&lui_list_lock);
	while (reader->count == 0 && s->created) {
		if (uio->uio_fmode & (FNDELAY | FNONBLOCK)) {
			error = EAGAIN;
			goto out;
		}
		if (cv_wait_sig(&s->cv, &s->lock) == 0) {
			error = EINTR;
			goto out;
		}
	}
	if (reader->count == 0) {
		error = ENODEV;
		goto out;
	}
	while (reader->count != 0 && uio->uio_resid >= event_size) {
		event = reader->events[reader->tail];
		reader->tail = (reader->tail + 1) % LUI_RING;
		reader->count--;
		mutex_exit(&s->lock);
		if (compat) {
			event32.sec = (int32_t)event.sec;
			event32.usec = (int32_t)event.usec;
			event32.type = event.type;
			event32.code = event.code;
			event32.value = event.value;
			error = uiomove(&event32, event_size, UIO_READ, uio);
		} else {
			error = uiomove(&event, event_size, UIO_READ, uio);
		}
		mutex_enter(&s->lock);
		if (error != 0)
			break;
	}
out:
	mutex_exit(&s->lock);
	return (error);
}

static int
lui_write(dev_t dev, struct uio *uio, cred_t *cr)
{
	lui_state_t *s;
	lui_reader_t *reader;
	lui_event_t event;
	lui_event32_t event32;
	lui_reader_t *wakers[LUI_MAX_READERS];
	uint_t nwake;
	boolean_t compat = get_udatamodel() != DATAMODEL_NATIVE;
	size_t event_size = compat ? sizeof (event32) : sizeof (event);
	struct timespec ts;
	int error = 0;
	if (getminor(dev) < LUI_CLONE_BASE ||
	    uio->uio_resid % event_size != 0)
		return (EINVAL);
	mutex_enter(&lui_list_lock);
	s = lui_find_producer(crgetzoneid(cr), getminor(dev));
	if (s == NULL) {
		mutex_exit(&lui_list_lock);
		return (ENODEV);
	}
	s->active++;
	mutex_exit(&lui_list_lock);
	while (uio->uio_resid >= event_size) {
		if (compat) {
			error = uiomove(&event32, event_size, UIO_WRITE, uio);
			event.type = event32.type;
			event.code = event32.code;
			event.value = event32.value;
		} else {
			error = uiomove(&event, event_size, UIO_WRITE, uio);
		}
		if (error != 0)
			break;
		gethrestime(&ts);
		event.sec = ts.tv_sec;
		event.usec = ts.tv_nsec / 1000;
		mutex_enter(&s->lock);
		if (!s->created) {
			mutex_exit(&s->lock);
			error = ENODEV;
			break;
		}
		if (event.type > LUI_EV_MAX ||
		    (s->ev[event.type / 8] & (1 << (event.type % 8))) == 0) {
			mutex_exit(&s->lock);
			continue;
		}
		if ((event.type == 1 && (event.code > LUI_KEY_MAX ||
		    (s->key[event.code / 8] &
		    (1 << (event.code % 8))) == 0)) ||
		    (event.type == 3 && (event.code > LUI_ABS_MAX ||
		    (s->abs[event.code / 8] &
		    (1 << (event.code % 8))) == 0))) {
			mutex_exit(&s->lock);
			continue;
		}
		if (event.type == 1) {
			if (event.value == 0)
				s->key_state[event.code / 8] &=
				    ~(1 << (event.code % 8));
			else
				s->key_state[event.code / 8] |=
				    1 << (event.code % 8);
		} else if (event.type == 3) {
			s->axes[event.code].value = event.value;
		}
		for (reader = s->reader_head; reader != NULL;
		    reader = reader->state_next) {
			if (reader->count == LUI_RING) {
				lui_event_t dropped = { 0 };
				dropped.sec = event.sec;
				dropped.usec = event.usec;
				dropped.type = 0; /* EV_SYN */
				dropped.code = 3; /* SYN_DROPPED */
				reader->head = reader->tail = reader->count = 0;
				reader->events[reader->head++] = dropped;
				reader->count++;
			}
			reader->events[reader->head] = event;
			reader->head = (reader->head + 1) % LUI_RING;
			reader->count++;
		}
		nwake = lui_collect_wakers(s, wakers);
		cv_broadcast(&s->cv);
		mutex_exit(&s->lock);
		lui_wake_readers(s, wakers, nwake, POLLIN | POLLRDNORM);
	}
	mutex_enter(&lui_list_lock);
	ASSERT(s->active != 0);
	s->active--;
	if (!s->producer && s->readers == 0 && s->active == 0)
		lui_free(s);
	mutex_exit(&lui_list_lock);
	return (error);
}

static int
lui_copyout(const void *src, size_t n, intptr_t arg, int mode)
{
	return (ddi_copyout(src, (void *)arg, n, mode) == 0 ? 0 : EFAULT);
}

/*
 * Linux creates a new evdev node for each device.  Steam/SDL and Wine with
 * udev disabled watch /dev/input with inotify and only probe a node when an
 * event names it; some SDL builds poll the directory mtime instead.  Our
 * event nodes are permanent, so mimic creation in the caller's /dev: bump the
 * directory mtime, and do a non-exclusive VOP_CREATE of the selected event
 * node, which sdev resolves to the existing node and inotify reports as
 * IN_CREATE "eventN"
 * (exactly what touch(1) does).  Best effort.
 */
static void
lui_announce_event(uint_t index)
{
	vnode_t *dvp, *vp;
	vattr_t va;
	char name[16];

	if (lookupname("/dev/input", UIO_SYSSPACE, FOLLOW, NULLVPP, &dvp) != 0)
		return;
	va.va_mask = AT_MTIME;
	gethrestime(&va.va_mtime);
	(void) VOP_SETATTR(dvp, &va, 0, kcred, NULL);
	bzero(&va, sizeof (va));
	va.va_mask = AT_TYPE | AT_MODE;
	va.va_type = VCHR;
	va.va_mode = 0666;
	(void) snprintf(name, sizeof (name), "event%u", index);
	if (VOP_CREATE(dvp, name, &va, NONEXCL, VREAD, &vp, kcred, 0,
	    NULL, NULL) == 0)
		VN_RELE(vp);
	VN_RELE(dvp);
}

static int
lui_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *cr, int *rv)
{
	lui_state_t *s;
	lui_reader_t *reader;
	boolean_t is_event = getminor(dev) >= LUI_READER_BASE;
	uint32_t nr = LUI_IOC_NR(cmd), size = LUI_IOC_SIZE(cmd);
	uint32_t version = 5;
	uint32_t code;
	uint8_t *bitmap = NULL;
	uint8_t output[96];
	size_t bytes = 0;
	size_t output_len = 0;
	boolean_t variable_result = B_FALSE;
	lui_abs_setup_t abs_setup;
	lui_setup_t setup;
	lui_reader_t *wakers[LUI_MAX_READERS];
	uint_t nwake = 0;
	boolean_t created = B_FALSE;
	uint_t created_index = 0;
	int error = 0;
	*rv = 0;
	if (!is_event && getminor(dev) >= LUI_CLONE_BASE &&
	    LUI_IOC_TYPE(cmd) == 'U' && nr == 0x03) {
		if (size != sizeof (setup))
			return (EINVAL);
		if (ddi_copyin((void *)arg, &setup, sizeof (setup), mode) != 0)
			return (EFAULT);
		setup.name[sizeof (setup.name) - 1] = 0;
	}
	if (!is_event && getminor(dev) >= LUI_CLONE_BASE &&
	    LUI_IOC_TYPE(cmd) == 'U' && nr == 0x04) {
		if (size != sizeof (abs_setup))
			return (EINVAL);
		if (ddi_copyin((void *)arg, &abs_setup,
		    sizeof (abs_setup), mode) != 0)
			return (EFAULT);
	}
	mutex_enter(&lui_list_lock);
	reader = is_event ? lui_find_reader(getminor(dev),
	    crgetzoneid(cr)) : NULL;
	s = is_event ? (reader != NULL ? reader->state : NULL) :
	    lui_find_producer(crgetzoneid(cr), getminor(dev));
	if (s == NULL) {
		mutex_exit(&lui_list_lock);
		return (ENOTTY);
	}
	mutex_enter(&s->lock);
	s->active++;
	mutex_exit(&lui_list_lock);
	if (is_event) {
		if (LUI_IOC_TYPE(cmd) != 'E') {
			error = ENOTTY;
			goto out;
		}
		switch (nr) {
		case 0x01: /* EVIOCGVERSION */
			if (size != sizeof (version)) {
				error = EINVAL;
				break;
			}
			bcopy(&version, output, sizeof (version));
			output_len = sizeof (version);
			break;
		case 0x02: /* EVIOCGID */
			if (size != sizeof (lui_id_t)) {
				error = EINVAL;
				break;
			}
			bcopy(&s->setup.id, output, sizeof (lui_id_t));
			output_len = sizeof (lui_id_t);
			break;
		case 0x06: /* EVIOCGNAME */
			output_len = MIN(size, strlen(s->setup.name) + 1);
			bcopy(s->setup.name, output, output_len);
			variable_result = B_TRUE;
			break;
		case 0x18: /* EVIOCGKEY */
			output_len = MIN(size, sizeof (s->key_state));
			bcopy(s->key_state, output, output_len);
			variable_result = B_TRUE;
			break;
		default:
			if (nr >= 0x20 && nr <= 0x3f) {
				code = nr - 0x20;
				switch (code) {
				case 0:
					bitmap = s->ev;
					bytes = sizeof (s->ev);
					break;
				case 1:
					bitmap = s->key;
					bytes = sizeof (s->key);
					break;
				case 3:
					bitmap = s->abs;
					bytes = sizeof (s->abs);
					break;
				case 21:
					bitmap = s->ff;
					bytes = sizeof (s->ff);
					break;
				default:
					bzero(output, sizeof (output));
					bytes = sizeof (output);
					break;
				}
				output_len = MIN(size, bytes);
				if (bitmap != NULL)
					bcopy(bitmap, output, output_len);
				variable_result = B_TRUE;
			} else if (nr >= 0x40 && nr <= 0x7f &&
			    (s->abs[(nr - 0x40) / 8] &
			    (1 << ((nr - 0x40) % 8))) != 0) {
				if (size != sizeof (lui_absinfo_t))
					error = EINVAL;
				else {
					bcopy(&s->axes[nr - 0x40], output,
					    sizeof (lui_absinfo_t));
					output_len = sizeof (lui_absinfo_t);
				}
			} else {
				error = ENOTTY;
			}
		}
		goto out;
	}
	if (!s->producer || LUI_IOC_TYPE(cmd) != 'U') {
		error = ENOTTY;
		goto out;
	}
	switch (nr) {
	case 0x2d: /* UI_GET_VERSION */
		bcopy(&version, output, sizeof (version));
		output_len = sizeof (version);
		break;
	case 0x01: /* UI_DEV_CREATE */
		if (s->created || s->readers != 0 || s->setup.name[0] == 0)
			error = EINVAL;
		else if ((s->ev[1 / 8] & (1 << (1 % 8))) == 0 ||
		    (s->ev[3 / 8] & (1 << (3 % 8))) == 0 ||
		    (s->key[LUI_BTN_SOUTH / 8] &
		    (1 << (LUI_BTN_SOUTH % 8))) == 0 ||
		    (s->abs[0] & 1) == 0)
			error = EOPNOTSUPP;
		else {
			s->created = created = B_TRUE;
			created_index = s->event_index;
		}
		break;
	case 0x02: /* UI_DEV_DESTROY */
		s->created = B_FALSE;
		bzero(s->key_state, sizeof (s->key_state));
		cv_broadcast(&s->cv);
		for (reader = s->reader_head; reader != NULL;
		    reader = reader->state_next) {
			reader->head = reader->tail = reader->count = 0;
		}
		nwake = lui_collect_wakers(s, wakers);
		break;
	case 0x03: /* UI_DEV_SETUP */
		if (s->created)
			error = EBUSY;
		else
			s->setup = setup;
		break;
	case 0x04: /* UI_ABS_SETUP */
		if (s->created || abs_setup.code > LUI_ABS_MAX)
			error = EINVAL;
		else
			s->axes[abs_setup.code] = abs_setup.absinfo;
		break;
	case 0x2c: /* UI_GET_SYSNAME */
		if (size == 0)
			error = EINVAL;
		else {
			(void) snprintf((char *)output, sizeof (output),
			    "input%u", s->event_index);
			output_len = MIN(size, strlen((char *)output) + 1);
			variable_result = B_TRUE;
		}
		break;
	case 0x64: bitmap = s->ev; bytes = LUI_EV_MAX; break;
	case 0x65: bitmap = s->key; bytes = LUI_KEY_MAX; break;
	case 0x67: bitmap = s->abs; bytes = LUI_ABS_MAX; break;
	case 0x6b: bitmap = s->ff; bytes = LUI_FF_MAX; break;
	default: error = ENOTTY; break;
	}
	if (bitmap != NULL) {
		code = (uint32_t)arg;
		if (code > bytes)
			error = EINVAL;
		else
			bitmap[code / 8] |= 1 << (code % 8);
	}
out:
	mutex_exit(&s->lock);
	if (nwake != 0)
		lui_wake_readers(s, wakers, nwake, POLLHUP);
	mutex_enter(&lui_list_lock);
	ASSERT(s->active != 0);
	s->active--;
	if (!s->producer && s->readers == 0 && s->active == 0)
		lui_free(s);
	mutex_exit(&lui_list_lock);
	if (created)
		lui_announce_event(created_index);
	if (error == 0 && output_len != 0)
		error = lui_copyout(output, output_len, arg, mode);
	if (error == 0 && variable_result)
		*rv = output_len;
	return (error);
}

static int
lui_poll(dev_t dev, short events, int anyyet, short *revents,
    struct pollhead **phpp)
{
	lui_state_t *s;
	lui_reader_t *reader;
	if (getminor(dev) < LUI_READER_BASE)
		return (ENXIO);
	mutex_enter(&lui_list_lock);
	reader = lui_find_reader(getminor(dev), getzoneid());
	if (reader == NULL) {
		mutex_exit(&lui_list_lock);
		return (ENODEV);
	}
	s = reader->state;
	mutex_enter(&s->lock);
	mutex_exit(&lui_list_lock);
	*revents = 0;
	if (reader->count > 0)
		*revents |= events & (POLLIN | POLLRDNORM);
	if (!s->created)
		*revents |= POLLHUP;
	if (*revents == 0 && !anyyet)
		*phpp = &reader->pollhead;
	mutex_exit(&s->lock);
	return (0);
}

static int
lui_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	uint_t index;
	char name[16];
	if (cmd != DDI_ATTACH || lui_dip != NULL)
		return (DDI_FAILURE);
	lui_producer_ids = id_space_create("lx_uinput producers", LUI_CLONE_BASE,
	    LUI_READER_BASE);
	if (lui_producer_ids == NULL)
		return (DDI_FAILURE);
	lui_reader_ids = id_space_create("lx_uinput readers", LUI_READER_BASE,
	    L_MAXMIN32 + 1);
	if (lui_reader_ids == NULL) {
		id_space_destroy(lui_producer_ids);
		lui_producer_ids = NULL;
		return (DDI_FAILURE);
	}
	if (ddi_create_minor_node(dip, "uinput", S_IFCHR,
	    LUI_PRODUCER, DDI_PSEUDO, 0) != DDI_SUCCESS) {
		id_space_destroy(lui_reader_ids);
		lui_reader_ids = NULL;
		id_space_destroy(lui_producer_ids);
		lui_producer_ids = NULL;
		return (DDI_FAILURE);
	}
	for (index = 0; index < LUI_MAX_DEVICES; index++) {
		(void) snprintf(name, sizeof (name), "event%u", index);
		if (ddi_create_minor_node(dip, name, S_IFCHR,
		    LUI_EVENT_BASE + index, DDI_PSEUDO, 0) != DDI_SUCCESS) {
			ddi_remove_minor_node(dip, NULL);
			id_space_destroy(lui_reader_ids);
			lui_reader_ids = NULL;
			id_space_destroy(lui_producer_ids);
			lui_producer_ids = NULL;
			return (DDI_FAILURE);
		}
	}
	lui_dip = dip;
	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

static int
lui_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	mutex_enter(&lui_list_lock);
	if (lui_states != NULL || lui_readers != NULL) {
		mutex_exit(&lui_list_lock);
		return (DDI_FAILURE);
	}
	mutex_exit(&lui_list_lock);
	ddi_remove_minor_node(dip, NULL);
	id_space_destroy(lui_reader_ids);
	lui_reader_ids = NULL;
	id_space_destroy(lui_producer_ids);
	lui_producer_ids = NULL;
	lui_dip = NULL;
	return (DDI_SUCCESS);
}

static int
lui_info(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	(void) dip;
	(void) arg;
	if (cmd == DDI_INFO_DEVT2DEVINFO)
		*result = lui_dip;
	else if (cmd == DDI_INFO_DEVT2INSTANCE)
		*result = (void *)0;
	else
		return (DDI_FAILURE);
	return (DDI_SUCCESS);
}

static struct cb_ops lui_cbops = {
	lui_open, lui_close, nodev, nodev, nodev, lui_read, lui_write,
	lui_ioctl, nodev, nodev, nodev, lui_poll, ddi_prop_op, NULL,
	D_NEW | D_MP
};
static struct dev_ops lui_devops = {
	DEVO_REV, 0, lui_info, nulldev, nulldev, lui_attach, lui_detach,
	nodev, &lui_cbops, NULL, nodev, ddi_quiesce_not_needed
};
static struct modldrv lui_modldrv = {
	&mod_driverops, "LX virtual gamepad input", &lui_devops
};
static struct modlinkage lui_linkage = { MODREV_1, &lui_modldrv, NULL };

int
_init(void)
{
	int error;
	mutex_init(&lui_list_lock, NULL, MUTEX_DEFAULT, NULL);
	if ((error = mod_install(&lui_linkage)) != 0)
		mutex_destroy(&lui_list_lock);
	return (error);
}

int
_fini(void)
{
	int error;
	if ((error = mod_remove(&lui_linkage)) == 0)
		mutex_destroy(&lui_list_lock);
	return (error);
}

int
_info(struct modinfo *info)
{
	return (mod_info(&lui_linkage, info));
}
