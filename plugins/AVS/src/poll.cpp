/*
Copyright (C) 2012-25 Miranda NG team (https://miranda-ng.org)
Copyright (C) 2006 Ricardo Pescuma Domenecci, Nightwish

This is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this file; see the file license.txt.  If
not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.
*/

#include "stdafx.h"

/*
It has 1 queue:
A queue to request items. One request is done at a time, REQUEST_WAIT_TIME miliseconts after it has beeing fired
ACKRESULT_STATUS. This thread only requests the avatar (and maybe add it to the cache queue)
*/

// Time to wait before re-requesting an avatar that failed
#define REQUEST_FAIL_WAIT_TIME (3 * 60 * 60 * 1000)

// Time to wait before re-requesting an avatar that received an wait for
#define REQUEST_WAITFOR_WAIT_TIME (30 * 60 * 1000)

// Number of mileseconds the threads wait until take a look if it is time to request another item
#define POOL_DELAY 1000

// Number of mileseconds the threads wait after a GAIR_WAITFOR is returned
#define REQUEST_DELAY 18000


// Prototypes ///////////////////////////////////////////////////////////////////////////

static HANDLE hRequestThread;
static void RequestThread(void *vParam);

extern void MakePathRelative(MCONTACT hContact, wchar_t *path);
int Proto_GetDelayAfterFail(const char *proto);
BOOL Proto_IsFetchingWhenProtoNotVisibleAllowed(const char *proto);
BOOL Proto_IsFetchingWhenContactOfflineAllowed(const char *proto);

// Functions ////////////////////////////////////////////////////////////////////////////

// Items with higher priority at end
static int QueueSortItems(const QueueItem *p1, const QueueItem *p2)
{
	return p2->check_time - p1->check_time; // sort backwards
}

static OBJLIST<QueueItem> queue(20, QueueSortItems);
static mir_cs cs;

void InitPolls()
{
	// Init request queue
	mir_forkthread(RequestThread);
}

void UninitPolls()
{
	if (hRequestThread)
		WaitForSingleObject(hRequestThread, INFINITE);

	queue.destroy();
}

// Return true if this protocol can have avatar requested
static BOOL PollProtocolCanHaveAvatar(const char *szProto)
{
	int pCaps = CallProtoService(szProto, PS_GETCAPS, PFLAGNUM_4, 0);
	int status = Proto_GetStatus(szProto);
	return (pCaps & PF4_AVATARS) && mir_strcmp(META_PROTO, szProto) &&
		((status > ID_STATUS_OFFLINE && status != ID_STATUS_INVISIBLE) || Proto_IsFetchingWhenProtoNotVisibleAllowed(szProto));
}

// Return true if this protocol has to be checked
static BOOL PollCheckProtocol(const char *szProto)
{
	return g_plugin.getByte(szProto, 1);
}

// Return true if this contact can have avatar requested
static BOOL PollContactCanHaveAvatar(MCONTACT hContact, const char *szProto)
{
	int status = db_get_w(hContact, szProto, "Status", ID_STATUS_OFFLINE);
	return (Proto_IsFetchingWhenContactOfflineAllowed(szProto) || status != ID_STATUS_OFFLINE) && Contact::OnList(hContact);
}

// Return true if this contact has to be checked
static BOOL PollCheckContact(MCONTACT hContact)
{
	return !db_get_b(hContact, "ContactPhoto", "Locked", 0) && FindAvatarInCache(hContact, false, true) != nullptr;
}

static void QueueRemove(MCONTACT hContact)
{
	mir_cslock lck(cs);

	for (auto &it : queue.rev_iter())
		if (it->hContact == hContact)
			queue.removeItem(&it);
}

// Add an contact to a queue
void QueueAdd(MCONTACT hContact, int waitTime)
{
	if (g_shutDown)
		return;

	mir_cslock lck(cs);

	// Only add if not exists yet
	for (auto &it : queue)
		if (it->hContact == hContact)
			return;

	QueueItem *item = new QueueItem;
	item->hContact = hContact;
	item->check_time = GetTickCount() + waitTime;
	queue.insert(item);
}

void ProcessAvatarInfo(MCONTACT hContact, int type, PROTO_AVATAR_INFORMATION *pai, const char *szProto)
{
	QueueRemove(hContact);

	if (type == GAIR_SUCCESS) {
		if (pai == nullptr)
			return;

		// Fix settings in DB
		db_unset(hContact, "ContactPhoto", "NeedUpdate");
		db_unset(hContact, "ContactPhoto", "RFile");
		if (!db_get_b(hContact, "ContactPhoto", "Locked", 0))
			db_unset(hContact, "ContactPhoto", "Backup");
		db_set_ws(hContact, "ContactPhoto", "File", pai->filename);
		db_set_w(hContact, "ContactPhoto", "Format", pai->format);

		if (pai->format == PA_FORMAT_PNG || pai->format == PA_FORMAT_JPEG
			|| pai->format == PA_FORMAT_ICON || pai->format == PA_FORMAT_BMP
			|| pai->format == PA_FORMAT_GIF) {
			// We can load it!
			MakePathRelative(hContact, pai->filename);
			ChangeAvatar(hContact, true, true, pai->format);
		}
		else // As we can't load it, notify but don't load
			ChangeAvatar(hContact, false, true, pai->format);
	}
	else if (type == GAIR_NOAVATAR) {
		db_unset(hContact, "ContactPhoto", "NeedUpdate");

		if (g_plugin.getByte("RemoveAvatarWhenContactRemoves", 1)) {
			// Delete settings
			db_unset(hContact, "ContactPhoto", "RFile");
			if (!db_get_b(hContact, "ContactPhoto", "Locked", 0))
				db_unset(hContact, "ContactPhoto", "Backup");
			db_unset(hContact, "ContactPhoto", "File");
			db_unset(hContact, "ContactPhoto", "Format");

			// Fix cache
			ChangeAvatar(hContact, false, true, 0);
		}
	}
	else if (type == GAIR_FAILED) {
		int wait = Proto_GetDelayAfterFail(szProto);
		if (wait > 0) {
			// Reschedule to request after needed time (and avoid requests before that)
			mir_cslock lock(cs);
			QueueRemove(hContact);
			QueueAdd(hContact, wait);
		}
	}
}

int FetchAvatarFor(MCONTACT hContact, char *szProto)
{
	int result = GAIR_NOAVATAR;

	if (szProto == nullptr)
		szProto = Proto_GetBaseAccountName(hContact);

	if (szProto != nullptr && PollProtocolCanHaveAvatar(szProto) && PollContactCanHaveAvatar(hContact, szProto)) {
		// Can have avatar, but must request it?
		if ((g_AvatarHistoryAvail && CallService(MS_AVATARHISTORY_ENABLED, hContact, 0)) || (PollCheckProtocol(szProto) && PollCheckContact(hContact))) {
			// Request it
			PROTO_AVATAR_INFORMATION ai = { 0 };
			ai.hContact = hContact;
			INT_PTR res = CallProtoService(szProto, PS_GETAVATARINFO, GAIF_FORCE, (LPARAM)&ai);
			if (res != CALLSERVICE_NOTFOUND)
				result = res;
			ProcessAvatarInfo(ai.hContact, result, &ai, szProto);
		}
	}

	return result;
}

static void RequestThread(void *)
{
	Thread_SetName("AVS: RequestThread");
	MThreadLock threadLock(hRequestThread);

	while (!g_shutDown) {
		mir_cslockfull lck(cs);

		if (queue.getCount() == 0) {
			// No items, so suspend thread
			lck.unlock();
			mir_sleep(POOL_DELAY);
			continue;
		}

		// Take a look at first item
		QueueItem &qi = queue[queue.getCount() - 1];
		if (qi.check_time > GetTickCount()) {
			// Not time to request yet, wait...
			lck.unlock();
			mir_sleep(POOL_DELAY);
			continue;
		}

		// Will request this item
		MCONTACT hContact = qi.hContact;
		queue.remove(queue.getCount() - 1);
		QueueRemove(hContact);
		lck.unlock();

		if (FetchAvatarFor(hContact) == GAIR_WAITFOR) {
			// Mark to not request this contact avatar for more 30 min
			lck.lock();
			QueueRemove(hContact);
			QueueAdd(hContact, REQUEST_WAITFOR_WAIT_TIME);
			lck.unlock();

			// Wait a little until requesting again
			mir_sleep(REQUEST_DELAY);
		}
	}
}
