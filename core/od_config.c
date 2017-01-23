
/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <machinarium.h>

#include "od_macro.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"

#define od_keyword(name, token) { name, sizeof(name) - 1, token }

static od_keyword_t od_config_keywords[] =
{
	/* main */
	od_keyword("odissey",         OD_LODISSEY),
	od_keyword("yes",             OD_LYES),
	od_keyword("no",              OD_LNO),
	od_keyword("on",              OD_LON),
	od_keyword("off",             OD_LOFF),
	od_keyword("daemonize",       OD_LDAEMONIZE),
	od_keyword("log_verbosity",   OD_LLOG_VERBOSITY),
	od_keyword("log_file",        OD_LLOG_FILE),
	od_keyword("pid_file",        OD_LPID_FILE),
	od_keyword("syslog",          OD_LSYSLOG),
	od_keyword("syslog_ident",    OD_LSYSLOG_IDENT),
	od_keyword("syslog_facility", OD_LSYSLOG_FACILITY),
	od_keyword("pooling",         OD_LPOOLING),
	/* listen */
	od_keyword("listen",          OD_LLISTEN),
	od_keyword("host",            OD_LHOST),
	od_keyword("port",            OD_LPORT),
	od_keyword("backlog",         OD_LBACKLOG),
	od_keyword("nodelay",         OD_LNODELAY),
	od_keyword("keepalive",       OD_LKEEPALIVE),
	od_keyword("workers",         OD_LWORKERS),
	od_keyword("client_max",      OD_LCLIENT_MAX),
	/* server */
	od_keyword("server",          OD_LSERVER),
	/* routing */
	od_keyword("routing",         OD_LROUTING),
	od_keyword("default",         OD_LDEFAULT),
	od_keyword("route",           OD_LROUTE),
	od_keyword("mode",            OD_LMODE),
	od_keyword("database",        OD_LDATABASE),
	od_keyword("user",            OD_LUSER),
	od_keyword("password",        OD_LPASSWORD),
	od_keyword("ttl",             OD_LTTL),
	od_keyword("pool_min",        OD_LPOOL_MIN),
	od_keyword("pool_max",        OD_LPOOL_MAX),
	/* users */
	od_keyword("users",           OD_LUSERS),
	{ NULL, 0,  0 }
};

void
od_configinit(od_config_t *config,
              od_log_t *log,
              od_scheme_t *scheme)
{
	od_lexinit(&config->lex);
	config->log = log;
	config->scheme = scheme;
}

int
od_configopen(od_config_t *config, char *file)
{
	/* read file */
	struct stat st;
	int rc = lstat(file, &st);
	if (rc == -1) {
		od_error(config->log, NULL, "failed to open config file '%s'",
		         file);
		return -1;
	}
	char *config_buf = malloc(st.st_size);
	if (config_buf == NULL) {
		od_error(config->log, NULL, "memory allocation error");
		return -1;
	}
	FILE *f = fopen(file, "r");
	if (f == NULL) {
		free(config_buf);
		od_error(config->log, NULL, "failed to open config file '%s'",
		         file);
		return -1;
	}
	rc = fread(config_buf, st.st_size, 1, f);
	fclose(f);
	if (rc != 1) {
		free(config_buf);
		od_error(config->log, NULL, "failed to open config file '%s'",
		         file);
		return -1;
	}
	od_lexopen(&config->lex, od_config_keywords, config_buf,
	           st.st_size);
	config->scheme->config_file = file;
	return 0;
}

void
od_configclose(od_config_t *config)
{
	od_lexfree(&config->lex);
}

static void
od_configerror(od_config_t *config, od_token_t *tk, char *fmt, ...)
{
	char msg[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	int line = config->lex.line;
	if (tk)
		line = tk->line;
	od_error(config->log, NULL, "%s:%d %s",
	         config->scheme->config_file, line, msg);
}

static int
od_confignext(od_config_t *config, int id, od_token_t **tk)
{
	od_token_t *tkp = NULL;
	int token = od_lexpop(&config->lex, &tkp);
	if (token == OD_LERROR) {
		od_configerror(config, NULL, "%s", config->lex.error);
		return -1;
	}
	if (tk) {
		*tk = tkp;
	}
	if (token != id) {
		if (id < 0xff && ispunct(id)) {
			od_configerror(config, tkp, "expected '%c'", id);
			return -1;
		}
		od_configerror(config, tkp, "expected '%s'",
		               od_lexname_of(&config->lex, id));
		return -1;
	}
	return 0;
}

static int
od_confignext_yes_no(od_config_t *config, od_token_t **tk)
{
	int rc;
	rc = od_lexpop(&config->lex, tk);
	if (rc == OD_LYES)
		return 1;
	if (rc == OD_LNO)
		return 0;
	od_configerror(config, *tk, "expected yes/no");
	return -1;
}

static int
od_configparse_listen(od_config_t *config)
{
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	od_token_t *tk;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* host */
		case OD_LHOST:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->host = tk->v.string;
			continue;
		/* port */
		case OD_LPORT:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->port = tk->v.num;
			continue;
		/* backlog */
		case OD_LBACKLOG:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->backlog = tk->v.num;
			continue;
		/* nodelay */
		case OD_LNODELAY:
			rc = od_confignext_yes_no(config, &tk);
			if (rc == -1)
				return -1;
			config->scheme->nodelay = rc;
			continue;
		/* keepalive */
		case OD_LKEEPALIVE:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->keepalive = tk->v.num;
			continue;
		/* client_max */
		case OD_LCLIENT_MAX:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->client_max = tk->v.num;
			continue;
		/* workers */
		case OD_LWORKERS:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->workers = tk->v.num;
			continue;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

static int
od_configparse_server(od_config_t *config)
{
	od_schemeserver_t *server =
		od_schemeserver_add(config->scheme);
	if (server == NULL)
		return -1;
	od_token_t *tk;
	int rc;
	/* name */
	if (od_confignext(config, OD_LSTRING, &tk) == -1)
		return -1;
	server->name = tk->v.string;
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* host */
		case OD_LHOST:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			server->host = tk->v.string;
			continue;
		/* port */
		case OD_LPORT:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			server->port = tk->v.num;
			continue;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

static int
od_configparse_route(od_config_t *config, od_token_t *name)
{
	od_schemeroute_t *route =
		od_schemeroute_add(config->scheme);
	if (route == NULL)
		return -1;
	if (name == NULL) {
		route->is_default = 1;
		route->target = "";
	} else {
		route->target = name->v.string;
	}
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	od_token_t *tk;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* route */
		case OD_LROUTE:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			route->route = tk->v.string;
			continue;
		/* client_max */
		case OD_LCLIENT_MAX:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			route->client_max = tk->v.num;
			continue;
		/* pool_min */
		case OD_LPOOL_MIN:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			route->pool_min = tk->v.num;
			continue;
		/* pool_max */
		case OD_LPOOL_MAX:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			route->pool_max = tk->v.num;
			continue;
		/* database */
		case OD_LDATABASE:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			route->database = tk->v.string;
			continue;
		/* user */
		case OD_LUSER:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			route->user = tk->v.string;
			continue;
		/* password */
		case OD_LPASSWORD:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			route->password = tk->v.string;
			continue;
		/* ttl */
		case OD_LTTL:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			route->ttl = tk->v.num;
			continue;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

static int
od_configparse_routing(od_config_t *config)
{
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	od_token_t *tk;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* mode */
		case OD_LMODE:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->routing = tk->v.string;
			continue;
		/* route (database name) */
		case OD_LSTRING:
			rc = od_configparse_route(config, tk);
			if (rc == -1)
				return -1;
			break;
		/* route default */
		case OD_LDEFAULT:
			rc = od_configparse_route(config, NULL);
			if (rc == -1)
				return -1;
			break;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

static int
od_configparse_user(od_config_t *config, od_token_t *name)
{
	od_schemeuser_t *user =
		od_schemeuser_add(config->scheme);
	if (user == NULL)
		return -1;
	user->user = name->v.string;
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	od_token_t *tk;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* password */
		case OD_LPASSWORD:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			user->password = tk->v.string;
			continue;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

static int
od_configparse_users(od_config_t *config)
{
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	od_token_t *tk;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* user (user name) */
		case OD_LSTRING:
			rc = od_configparse_user(config, tk);
			if (rc == -1)
				return -1;
			break;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}

int
od_configparse(od_config_t *config)
{
	od_token_t *tk;
	if (od_confignext(config, OD_LODISSEY, NULL) == -1)
		return -1;
	if (od_confignext(config, '{', NULL) == -1)
		return -1;
	int rc;
	int eof = 0;
	while (! eof)
	{
		rc = od_lexpop(&config->lex, &tk);
		switch (rc) {
		/* daemonize */
		case OD_LDAEMONIZE:
			rc = od_confignext_yes_no(config, &tk);
			if (rc == -1)
				return -1;
			config->scheme->daemonize = rc;
			continue;
		/* log_verbosity */
		case OD_LLOG_VERBOSITY:
			if (od_confignext(config, OD_LNUMBER, &tk) == -1)
				return -1;
			config->scheme->log_verbosity = tk->v.num;
			continue;
		/* log_file */
		case OD_LLOG_FILE:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->log_file = tk->v.string;
			continue;
		/* pid_file */
		case OD_LPID_FILE:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->pid_file = tk->v.string;
			continue;
		/* syslog */
		case OD_LSYSLOG:
			rc = od_confignext_yes_no(config, &tk);
			if (rc == -1)
				return -1;
			config->scheme->syslog = rc;
			continue;
		/* syslog_ident */
		case OD_LSYSLOG_IDENT:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->syslog_ident = tk->v.string;
			continue;
		/* syslog_facility */
		case OD_LSYSLOG_FACILITY:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->syslog_facility = tk->v.string;
			continue;
		/* pooling */
		case OD_LPOOLING:
			if (od_confignext(config, OD_LSTRING, &tk) == -1)
				return -1;
			config->scheme->pooling = tk->v.string;
			continue;
		/* listen */
		case OD_LLISTEN:
			rc = od_configparse_listen(config);
			if (rc == -1)
				return -1;
			continue;
		/* server */
		case OD_LSERVER:
			rc = od_configparse_server(config);
			if (rc == -1)
				return -1;
			continue;
		/* routing */
		case OD_LROUTING:
			rc = od_configparse_routing(config);
			if (rc == -1)
				return -1;
			continue;
		/* users */
		case OD_LUSERS:
			rc = od_configparse_users(config);
			if (rc == -1)
				return -1;
			continue;
		case OD_LEOF:
			od_configerror(config, tk, "unexpected end of config file");
			return -1;
		case '}':
			eof = 1;
			continue;
		default:
			od_configerror(config, tk, "unknown option");
			return -1;
		}
	}
	return 0;
}
