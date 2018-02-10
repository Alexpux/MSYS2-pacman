/*
 *  callback.c
 *
 *  Copyright (c) 2006-2016 Pacman Development Team <pacman-dev@archlinux.org>
 *  Copyright (c) 2002-2006 by Judd Vinet <jvinet@zeroflux.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h> /* gettimeofday */
#include <sys/types.h> /* off_t */
#include <stdint.h> /* int64_t */
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <limits.h> /* UINT_MAX */

#include <alpm.h>

/* pacman */
#include "pacman.h"
#include "callback.h"
#include "util.h"
#include "conf.h"

/* download progress bar */
static off_t list_xfered = 0.0;
static off_t list_total = 0.0;

/* delayed output during progress bar */
static int on_progress = 0;
static alpm_list_t *output = NULL;

/* update speed for the fill_progress based functions */
#define UPDATE_SPEED_MS 200

#if !defined(CLOCK_MONOTONIC_COARSE) && defined(CLOCK_MONOTONIC)
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

static int64_t get_time_ms(void)
{
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) && defined(CLOCK_MONOTONIC_COARSE)
	struct timespec ts = {0, 0};
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
#else
	/* darwin doesn't support clock_gettime, fallback to gettimeofday */
	struct timeval tv = {0, 0};
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
#endif
}

/**
 * Silly little helper function, determines if the caller needs a visual update
 * since the last time this function was called.
 * This is made for the two progress bar functions, to prevent flicker.
 * @param first_call 1 on first call for initialization purposes, 0 otherwise
 * @return number of milliseconds since last call
 */
static int64_t get_update_timediff(int first_call)
{
	int64_t retval = 0;
	static int64_t last_time = 0;

	/* on first call, simply set the last time and return */
	if(first_call) {
		last_time = get_time_ms();
	} else {
		int64_t this_time = get_time_ms();
		retval = this_time - last_time;

		/* do not update last_time if interval was too short */
		if(retval < 0 || retval >= UPDATE_SPEED_MS) {
			last_time = this_time;
		}
	}

	return retval;
}

/* refactored from cb_trans_progress */
static void fill_progress(const int bar_percent, const int disp_percent,
		const int proglen)
{
	/* 9 = 1 space + 1 [ + 1 ] + 5 for percent + 1 blank */
	/* Without the single blank at the end, carriage return wouldn't
	 * work properly on most windows terminals.
	 */
	const int hashlen = proglen > 9 ? proglen - 9 : 0;
	const int hash = bar_percent * hashlen / 100;
	static int lasthash = 0, mouth = 0;
	int i;

	if(bar_percent == 0) {
		lasthash = 0;
		mouth = 0;
	}

	if(hashlen > 0) {
		fputs(" [", stdout);
		for(i = hashlen; i > 0; --i) {
			/* if special progress bar enabled */
			if(config->chomp) {
				if(i > hashlen - hash) {
					putchar('-');
				} else if(i == hashlen - hash) {
					if(lasthash == hash) {
						if(mouth) {
							fputs("\033[1;33mC\033[m", stdout);
						} else {
							fputs("\033[1;33mc\033[m", stdout);
						}
					} else {
						lasthash = hash;
						mouth = mouth == 1 ? 0 : 1;
						if(mouth) {
							fputs("\033[1;33mC\033[m", stdout);
						} else {
							fputs("\033[1;33mc\033[m", stdout);
						}
					}
				} else if(i % 3 == 0) {
					fputs("\033[0;37mo\033[m", stdout);
				} else {
					fputs("\033[0;37m \033[m", stdout);
				}
			} /* else regular progress bar */
			else if(i > hashlen - hash) {
				putchar('#');
			} else {
				putchar('-');
			}
		}
		putchar(']');
	}
	/* print display percent after progress bar */
	/* 5 = 1 space + 3 digits + 1 % */
	if(proglen >= 5) {
		printf(" %3d%%", disp_percent);
	}

	if(bar_percent == 100) {
		putchar('\n');
	} else {
		putchar('\r');
	}
	fflush(stdout);
}

static int number_length(size_t n)
{
	int digits = 1;
	while((n /= 10)) {
		++digits;
	}

	return digits;
}

/* callback to handle messages/notifications from libalpm transactions */
void cb_event(alpm_event_t *event)
{
	if(config->print) {
		return;
	}
	switch(event->type) {
		case ALPM_EVENT_HOOK_START:
			if(event->hook.when == ALPM_HOOK_PRE_TRANSACTION) {
				colon_printf(_("Running pre-transaction hooks...\n"));
			} else {
				colon_printf(_("Running post-transaction hooks...\n"));
			}
			break;
		case ALPM_EVENT_HOOK_RUN_START:
			{
				alpm_event_hook_run_t *e = &event->hook_run;
				int digits = number_length(e->total);
				printf("(%*zu/%*zu) %s\n", digits, e->position,
						digits, e->total, 
						e->desc ? e->desc : e->name);
			}
			break;
		case ALPM_EVENT_CHECKDEPS_START:
			printf(_("checking dependencies...\n"));
			break;
		case ALPM_EVENT_FILECONFLICTS_START:
			if(config->noprogressbar) {
				printf(_("checking for file conflicts...\n"));
			}
			break;
		case ALPM_EVENT_RESOLVEDEPS_START:
			printf(_("resolving dependencies...\n"));
			break;
		case ALPM_EVENT_INTERCONFLICTS_START:
			printf(_("looking for conflicting packages...\n"));
			break;
		case ALPM_EVENT_TRANSACTION_START:
			colon_printf(_("Processing package changes...\n"));
			break;
		case ALPM_EVENT_PACKAGE_OPERATION_START:
			if(config->noprogressbar) {
				alpm_event_package_operation_t *e = &event->package_operation;
				switch(e->operation) {
					case ALPM_PACKAGE_INSTALL:
						printf(_("installing %s...\n"), alpm_pkg_get_name(e->newpkg));
						break;
					case ALPM_PACKAGE_UPGRADE:
						printf(_("upgrading %s...\n"), alpm_pkg_get_name(e->newpkg));
						break;
					case ALPM_PACKAGE_REINSTALL:
						printf(_("reinstalling %s...\n"), alpm_pkg_get_name(e->newpkg));
						break;
					case ALPM_PACKAGE_DOWNGRADE:
						printf(_("downgrading %s...\n"), alpm_pkg_get_name(e->newpkg));
						break;
					case ALPM_PACKAGE_REMOVE:
						printf(_("removing %s...\n"), alpm_pkg_get_name(e->oldpkg));
						break;
				}
			}
			break;
		case ALPM_EVENT_PACKAGE_OPERATION_DONE:
			{
				alpm_event_package_operation_t *e = &event->package_operation;
				switch(e->operation) {
					case ALPM_PACKAGE_INSTALL:
						display_optdepends(e->newpkg);
						break;
					case ALPM_PACKAGE_UPGRADE:
					case ALPM_PACKAGE_DOWNGRADE:
						display_new_optdepends(e->oldpkg, e->newpkg);
						break;
					case ALPM_PACKAGE_REINSTALL:
					case ALPM_PACKAGE_REMOVE:
						break;
				}
			}
			break;
		case ALPM_EVENT_INTEGRITY_START:
			if(config->noprogressbar) {
				printf(_("checking package integrity...\n"));
			}
			break;
		case ALPM_EVENT_KEYRING_START:
			if(config->noprogressbar) {
				printf(_("checking keyring...\n"));
			}
			break;
		case ALPM_EVENT_KEY_DOWNLOAD_START:
			printf(_("downloading required keys...\n"));
			break;
		case ALPM_EVENT_LOAD_START:
			if(config->noprogressbar) {
				printf(_("loading package files...\n"));
			}
			break;
		case ALPM_EVENT_DELTA_INTEGRITY_START:
			printf(_("checking delta integrity...\n"));
			break;
		case ALPM_EVENT_DELTA_PATCHES_START:
			printf(_("applying deltas...\n"));
			break;
		case ALPM_EVENT_DELTA_PATCH_START:
			printf(_("generating %s with %s... "),
					event->delta_patch.delta->to,
					event->delta_patch.delta->delta);
			break;
		case ALPM_EVENT_DELTA_PATCH_DONE:
			printf(_("success!\n"));
			break;
		case ALPM_EVENT_DELTA_PATCH_FAILED:
			printf(_("failed.\n"));
			break;
		case ALPM_EVENT_SCRIPTLET_INFO:
			fputs(event->scriptlet_info.line, stdout);
			break;
		case ALPM_EVENT_RETRIEVE_START:
			colon_printf(_("Retrieving packages...\n"));
			break;
		case ALPM_EVENT_DISKSPACE_START:
			if(config->noprogressbar) {
				printf(_("checking available disk space...\n"));
			}
			break;
		case ALPM_EVENT_OPTDEP_REMOVAL:
			{
				alpm_event_optdep_removal_t *e = &event->optdep_removal;
				char *dep_string = alpm_dep_compute_string(e->optdep);
				colon_printf(_("%s optionally requires %s\n"),
						alpm_pkg_get_name(e->pkg),
						dep_string);
				free(dep_string);
			}
			break;
		case ALPM_EVENT_DATABASE_MISSING:
			if(!config->op_s_sync) {
				pm_printf(ALPM_LOG_WARNING,
					"database file for '%s' does not exist\n",
					event->database_missing.dbname);
			}
			break;
		case ALPM_EVENT_PACNEW_CREATED:
			{
				alpm_event_pacnew_created_t *e = &event->pacnew_created;
				if(on_progress) {
					char *string = NULL;
					pm_sprintf(&string, ALPM_LOG_WARNING, _("%s installed as %s.pacnew\n"),
							e->file, e->file);
					if(string != NULL) {
						output = alpm_list_add(output, string);
					}
				} else {
					pm_printf(ALPM_LOG_WARNING, _("%s installed as %s.pacnew\n"),
							e->file, e->file);
				}
			}
			break;
		case ALPM_EVENT_PACSAVE_CREATED:
			{
				alpm_event_pacsave_created_t *e = &event->pacsave_created;
				if(on_progress) {
					char *string = NULL;
					pm_sprintf(&string, ALPM_LOG_WARNING, _("%s saved as %s.pacsave\n"),
							e->file, e->file);
					if(string != NULL) {
						output = alpm_list_add(output, string);
					}
				} else {
					pm_printf(ALPM_LOG_WARNING, _("%s saved as %s.pacsave\n"),
							e->file, e->file);
				}
			}
			break;
		/* all the simple done events, with fallthrough for each */
		case ALPM_EVENT_FILECONFLICTS_DONE:
		case ALPM_EVENT_CHECKDEPS_DONE:
		case ALPM_EVENT_RESOLVEDEPS_DONE:
		case ALPM_EVENT_INTERCONFLICTS_DONE:
		case ALPM_EVENT_TRANSACTION_DONE:
		case ALPM_EVENT_INTEGRITY_DONE:
		case ALPM_EVENT_KEYRING_DONE:
		case ALPM_EVENT_KEY_DOWNLOAD_DONE:
		case ALPM_EVENT_LOAD_DONE:
		case ALPM_EVENT_DELTA_INTEGRITY_DONE:
		case ALPM_EVENT_DELTA_PATCHES_DONE:
		case ALPM_EVENT_DISKSPACE_DONE:
		case ALPM_EVENT_RETRIEVE_DONE:
		case ALPM_EVENT_RETRIEVE_FAILED:
		case ALPM_EVENT_HOOK_DONE:
		case ALPM_EVENT_HOOK_RUN_DONE:
		/* we can safely ignore those as well */
		case ALPM_EVENT_PKGDOWNLOAD_START:
		case ALPM_EVENT_PKGDOWNLOAD_DONE:
		case ALPM_EVENT_PKGDOWNLOAD_FAILED:
			/* nothing */
			break;
	}
	fflush(stdout);
}

/* callback to handle questions from libalpm transactions (yes/no) */
void cb_question(alpm_question_t *question)
{
	if(config->print) {
		switch(question->type) {
			case ALPM_QUESTION_INSTALL_IGNOREPKG:
			case ALPM_QUESTION_REPLACE_PKG:
				question->any.answer = 1;
				break;
			default:
				question->any.answer = 0;
				break;
		}
		return;
	}
	switch(question->type) {
		case ALPM_QUESTION_INSTALL_IGNOREPKG:
			{
				alpm_question_install_ignorepkg_t *q = &question->install_ignorepkg;
				if(!config->op_s_downloadonly) {
					q->install = yesno(_("%s is in IgnorePkg/IgnoreGroup. Install anyway?"),
							alpm_pkg_get_name(q->pkg));
				} else {
					q->install = 1;
				}
			}
			break;
		case ALPM_QUESTION_REPLACE_PKG:
			{
				alpm_question_replace_t *q = &question->replace;
				q->replace = yesno(_("Replace %s with %s/%s?"),
						alpm_pkg_get_name(q->oldpkg),
						alpm_db_get_name(q->newdb),
						alpm_pkg_get_name(q->newpkg));
			}
			break;
		case ALPM_QUESTION_CONFLICT_PKG:
			{
				alpm_question_conflict_t *q = &question->conflict;
				/* print conflict only if it contains new information */
				if(strcmp(q->conflict->package1, q->conflict->reason->name) == 0
						|| strcmp(q->conflict->package2, q->conflict->reason->name) == 0) {
					q->remove = yesno(_("%s and %s are in conflict. Remove %s?"),
							q->conflict->package1,
							q->conflict->package2,
							q->conflict->package2);
				} else {
					q->remove = yesno(_("%s and %s are in conflict (%s). Remove %s?"),
							q->conflict->package1,
							q->conflict->package2,
							q->conflict->reason->name,
							q->conflict->package2);
				}
			}
			break;
		case ALPM_QUESTION_REMOVE_PKGS:
			{
				alpm_question_remove_pkgs_t *q = &question->remove_pkgs;
				alpm_list_t *namelist = NULL, *i;
				size_t count = 0;
				for(i = q->packages; i; i = i->next) {
					namelist = alpm_list_add(namelist,
							(char *)alpm_pkg_get_name(i->data));
					count++;
				}
				colon_printf(_n(
							"The following package cannot be upgraded due to unresolvable dependencies:\n",
							"The following packages cannot be upgraded due to unresolvable dependencies:\n",
							count));
				list_display("     ", namelist, getcols());
				printf("\n");
				q->skip = yesno(_n(
							"Do you want to skip the above package for this upgrade?",
							"Do you want to skip the above packages for this upgrade?",
							count));
				alpm_list_free(namelist);
			}
			break;
		case ALPM_QUESTION_SELECT_PROVIDER:
			{
				alpm_question_select_provider_t *q = &question->select_provider;
				size_t count = alpm_list_count(q->providers);
				char *depstring = alpm_dep_compute_string(q->depend);
				colon_printf(_n("There is %zu provider available for %s\n",
						"There are %zu providers available for %s:\n", count),
						count, depstring);
				free(depstring);
				select_display(q->providers);
				q->use_index = select_question(count);
			}
			break;
		case ALPM_QUESTION_CORRUPTED_PKG:
			{
				alpm_question_corrupted_t *q = &question->corrupted;
				q->remove = yesno(_("File %s is corrupted (%s).\n"
							"Do you want to delete it?"),
						q->filepath,
						alpm_strerror(q->reason));
			}
			break;
		case ALPM_QUESTION_IMPORT_KEY:
			{
				alpm_question_import_key_t *q = &question->import_key;
				char created[12];
				time_t time = (time_t)q->key->created;
				strftime(created, 12, "%Y-%m-%d", localtime(&time));

				if(q->key->revoked) {
					q->import = yesno(_("Import PGP key %u%c/%s, \"%s\", created: %s (revoked)?"),
							q->key->length, q->key->pubkey_algo, q->key->fingerprint, q->key->uid, created);
				} else {
					q->import = yesno(_("Import PGP key %u%c/%s, \"%s\", created: %s?"),
							q->key->length, q->key->pubkey_algo, q->key->fingerprint, q->key->uid, created);
				}
			}
			break;
	}
	if(config->noask) {
		if(config->ask & question->type) {
			/* inverse the default answer */
			question->any.answer = !question->any.answer;
		}
	}
}

/* callback to handle display of transaction progress */
void cb_progress(alpm_progress_t event, const char *pkgname, int percent,
                       size_t howmany, size_t current)
{
	static int prevpercent;
	static size_t prevcurrent;
	/* size of line to allocate for text printing (e.g. not progressbar) */
	int infolen;
	int digits, textlen;
	char *opr = NULL;
	/* used for wide character width determination and printing */
	int len, wclen, wcwid, padwid;
	wchar_t *wcstr;

	const unsigned short cols = getcols();

	if(config->noprogressbar || cols == 0) {
		return;
	}

	if(percent == 0) {
		get_update_timediff(1);
	} else if(percent == 100) {
		/* no need for timediff update, but unconditionally continue unless we
		 * already completed on a previous call */
		if(prevpercent == 100) {
			return;
		}
	} else {
		if(current != prevcurrent) {
			/* update always */
		} else if(!pkgname || percent == prevpercent ||
				get_update_timediff(0) < UPDATE_SPEED_MS) {
			/* only update the progress bar when we have a package name, the
			 * percentage has changed, and it has been long enough. */
			return;
		}
	}

	prevpercent = percent;
	prevcurrent = current;

	/* set text of message to display */
	switch(event) {
		case ALPM_PROGRESS_ADD_START:
			opr = _("installing");
			break;
		case ALPM_PROGRESS_UPGRADE_START:
			opr = _("upgrading");
			break;
		case ALPM_PROGRESS_DOWNGRADE_START:
			opr = _("downgrading");
			break;
		case ALPM_PROGRESS_REINSTALL_START:
			opr = _("reinstalling");
			break;
		case ALPM_PROGRESS_REMOVE_START:
			opr = _("removing");
			break;
		case ALPM_PROGRESS_CONFLICTS_START:
			opr = _("checking for file conflicts");
			break;
		case ALPM_PROGRESS_DISKSPACE_START:
			opr = _("checking available disk space");
			break;
		case ALPM_PROGRESS_INTEGRITY_START:
			opr = _("checking package integrity");
			break;
		case ALPM_PROGRESS_KEYRING_START:
			opr = _("checking keys in keyring");
			break;
		case ALPM_PROGRESS_LOAD_START:
			opr = _("loading package files");
			break;
		default:
			return;
	}

	infolen = cols * 6 / 10;
	if(infolen < 50) {
		infolen = 50;
	}

	/* find # of digits in package counts to scale output */
	digits = number_length(howmany);

	/* determine room left for non-digits text [not ( 1/12) part] */
	textlen = infolen - 3 /* (/) */ - (2 * digits) - 1 /* space */;

	/* In order to deal with characters from all locales, we have to worry
	 * about wide characters and their column widths. A lot of stuff is
	 * done here to figure out the actual number of screen columns used
	 * by the output, and then pad it accordingly so we fill the terminal.
	 */
	/* len = opr len + pkgname len (if available) + space + null */
	len = strlen(opr) + ((pkgname) ? strlen(pkgname) : 0) + 2;
	wcstr = calloc(len, sizeof(wchar_t));
	/* print our strings to the alloc'ed memory */
#if defined(HAVE_SWPRINTF)
	wclen = swprintf(wcstr, len, L"%s %s", opr, pkgname);
#else
	/* because the format string was simple, we can easily do this without
	 * using swprintf, although it is probably not as safe/fast. The max
	 * chars we can copy is decremented each time by subtracting the length
	 * of the already printed/copied wide char string. */
	wclen = mbstowcs(wcstr, opr, len);
	wclen += mbstowcs(wcstr + wclen, " ", len - wclen);
	wclen += mbstowcs(wcstr + wclen, pkgname, len - wclen);
#endif
	wcwid = wcswidth(wcstr, wclen);
	padwid = textlen - wcwid;
	/* if padwid is < 0, we need to trim the string so padwid = 0 */
	if(padwid < 0) {
		int i = textlen - 3;
		wchar_t *p = wcstr;
		/* grab the max number of char columns we can fill */
		while(i - wcwidth(*p) > 0) {
			i -= wcwidth(*p);
			p++;
		}
		/* then add the ellipsis and fill out any extra padding */
		wcscpy(p, L"...");
		padwid = i;

	}

	printf("(%*zu/%*zu) %ls%-*s", digits, current,
			digits, howmany, wcstr, padwid, "");

	free(wcstr);

	/* call refactored fill progress function */
	fill_progress(percent, percent, cols - infolen);

	if(percent == 100) {
		alpm_list_t *i = NULL;
		on_progress = 0;
		fflush(stdout);
		for(i = output; i; i = i->next) {
			fputs((const char *)i->data, stderr);
		}
		fflush(stderr);
		FREELIST(output);
	} else {
		on_progress = 1;
	}
}

/* callback to handle receipt of total download value */
void cb_dl_total(off_t total)
{
	list_total = total;
	/* if we get a 0 value, it means this list has finished downloading,
	 * so clear out our list_xfered as well */
	if(total == 0) {
		list_xfered = 0;
	}
}

/* callback to handle display of download progress */
void cb_dl_progress(const char *filename, off_t file_xfered, off_t file_total)
{
	static double rate_last;
	static off_t xfered_last;
	static int64_t initial_time = 0;
	int infolen;
	int filenamelen;
	char *fname, *p;
	/* used for wide character width determination and printing */
	int len, wclen, wcwid, padwid;
	wchar_t *wcfname;

	int totaldownload = 0;
	off_t xfered, total;
	double rate = 0.0;
	unsigned int eta_h = 0, eta_m = 0, eta_s = 0;
	double rate_human, xfered_human;
	const char *rate_label, *xfered_label;
	int file_percent = 0, total_percent = 0;

	const unsigned short cols = getcols();

	if(config->noprogressbar || cols == 0 || file_total == -1) {
		if(file_xfered == 0) {
			printf(_("downloading %s...\n"), filename);
			fflush(stdout);
		}
		return;
	}

	infolen = cols * 6 / 10;
	if(infolen < 50) {
		infolen = 50;
	}
	/* only use TotalDownload if enabled and we have a callback value */
	if(config->totaldownload && list_total) {
		/* sanity check */
		if(list_xfered + file_total <= list_total) {
			totaldownload = 1;
		} else {
			/* bogus values : don't enable totaldownload and reset */
			list_xfered = 0;
			list_total = 0;
		}
	}

	if(totaldownload) {
		xfered = list_xfered + file_xfered;
		total = list_total;
	} else {
		xfered = file_xfered;
		total = file_total;
	}

	/* bogus values : stop here */
	if(xfered > total || xfered < 0) {
		return;
	}

	/* this is basically a switch on xfered: 0, total, and
	 * anything else */
	if(file_xfered == 0) {
		/* set default starting values, ensure we only call this once
		 * if TotalDownload is enabled */
		if(!totaldownload || (totaldownload && list_xfered == 0)) {
			initial_time = get_time_ms();
			xfered_last = (off_t)0;
			rate_last = 0.0;
			get_update_timediff(1);
		}
	} else if(file_xfered == file_total) {
		/* compute final values */
		int64_t timediff = get_time_ms() - initial_time;
		if(timediff > 0) {
			rate = (double)xfered / (timediff / 1000.0);
			/* round elapsed time (in ms) to the nearest second */
			eta_s = (unsigned int)(timediff + 500) / 1000;
		} else {
			eta_s = 0;
		}
	} else {
		/* compute current average values */
		int64_t timediff = get_update_timediff(0);

		if(timediff < UPDATE_SPEED_MS) {
			/* return if the calling interval was too short */
			return;
		}
		rate = (double)(xfered - xfered_last) / (timediff / 1000.0);
		/* average rate to reduce jumpiness */
		rate = (rate + 2 * rate_last) / 3;
		if(rate > 0.0) {
			eta_s = (total - xfered) / rate;
		} else {
			eta_s = UINT_MAX;
		}
		rate_last = rate;
		xfered_last = xfered;
	}

	if(file_total) {
		file_percent = (file_xfered * 100) / file_total;
	} else {
		file_percent = 100;
	}

	if(totaldownload) {
		total_percent = ((list_xfered + file_xfered) * 100) /
			list_total;

		/* if we are at the end, add the completed file to list_xfered */
		if(file_xfered == file_total) {
			list_xfered += file_total;
		}
	}

	/* fix up time for display */
	eta_h = eta_s / 3600;
	eta_s -= eta_h * 3600;
	eta_m = eta_s / 60;
	eta_s -= eta_m * 60;

	len = strlen(filename);
	fname = malloc(len + 1);
	memcpy(fname, filename, len);
	/* strip package or DB extension for cleaner look */
	if((p = strstr(fname, ".pkg")) || (p = strstr(fname, ".db")) || (p = strstr(fname, ".files"))) {
		/* tack on a .sig suffix for signatures */
		if(memcmp(&filename[len - 4], ".sig", 4) == 0) {
			memcpy(p, ".sig", 4);

			/* adjust length for later calculations */
			len = p - fname + 4;
		} else {
			len = p - fname;
		}
	}
	fname[len] = '\0';

	/* 1 space + filenamelen + 1 space + 6 for size + 1 space + 3 for label +
	 * + 2 spaces + 4 for rate + 1 for label + 2 for /s + 1 space +
	 * 8 for eta, gives us the magic 30 */
	filenamelen = infolen - 30;
	/* see printf() code, we omit 'HH:' in these conditions */
	if(eta_h == 0 || eta_h >= 100) {
		filenamelen += 3;
	}

	/* In order to deal with characters from all locales, we have to worry
	 * about wide characters and their column widths. A lot of stuff is
	 * done here to figure out the actual number of screen columns used
	 * by the output, and then pad it accordingly so we fill the terminal.
	 */
	/* len = filename len + null */
	wcfname = calloc(len + 1, sizeof(wchar_t));
	wclen = mbstowcs(wcfname, fname, len);
	wcwid = wcswidth(wcfname, wclen);
	padwid = filenamelen - wcwid;
	/* if padwid is < 0, we need to trim the string so padwid = 0 */
	if(padwid < 0) {
		int i = filenamelen - 3;
		wchar_t *wcp = wcfname;
		/* grab the max number of char columns we can fill */
		while(i > 0 && wcwidth(*wcp) < i) {
			i -= wcwidth(*wcp);
			wcp++;
		}
		/* then add the ellipsis and fill out any extra padding */
		wcscpy(wcp, L"...");
		padwid = i;

	}

	rate_human = humanize_size((off_t)rate, '\0', -1, &rate_label);
	xfered_human = humanize_size(xfered, '\0', -1, &xfered_label);

	printf(" %ls%-*s ", wcfname, padwid, "");
	/* We will show 1.62M/s, 11.6M/s, but 116K/s and 1116K/s */
	if(rate_human < 9.995) {
		printf("%6.1f %3s  %4.2f%c/s ",
				xfered_human, xfered_label, rate_human, rate_label[0]);
	} else if(rate_human < 99.95) {
		printf("%6.1f %3s  %4.1f%c/s ",
				xfered_human, xfered_label, rate_human, rate_label[0]);
	} else {
		printf("%6.1f %3s  %4.f%c/s ",
				xfered_human, xfered_label, rate_human, rate_label[0]);
	}
	if(eta_h == 0) {
		printf("%02u:%02u", eta_m, eta_s);
	} else if(eta_h < 100) {
		printf("%02u:%02u:%02u", eta_h, eta_m, eta_s);
	} else {
		fputs("--:--", stdout);
	}

	free(fname);
	free(wcfname);

	if(totaldownload) {
		fill_progress(file_percent, total_percent, cols - infolen);
	} else {
		fill_progress(file_percent, file_percent, cols - infolen);
	}
	return;
}

/* Callback to handle notifications from the library */
void cb_log(alpm_loglevel_t level, const char *fmt, va_list args)
{
	if(!fmt || strlen(fmt) == 0) {
		return;
	}

	if(on_progress) {
		char *string = NULL;
		pm_vasprintf(&string, level, fmt, args);
		if(string != NULL) {
			output = alpm_list_add(output, string);
		}
	} else {
		pm_vfprintf(stderr, level, fmt, args);
	}
}

/* vim: set noet: */
