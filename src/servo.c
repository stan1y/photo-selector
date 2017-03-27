#include "servo.h"
#include "util.h"
#include "assets.h"

struct servo_config *CONFIG;

struct http_state   servo_session_states[] = {
    { "REQ_STATE_C_SESSION",  state_connect_session },
    { "REQ_STATE_Q_SESSION",  state_query_session },
    { "REQ_STATE_W_SESSION",  state_wait_session },
    { "REQ_STATE_R_SESSION",  state_read_session },

    { "REQ_STATE_C_ITEM",     state_connect_item },
    { "REQ_STATE_Q_ITEM",	  state_query_item },
    { "REQ_STATE_W_ITEM",	  state_wait_item },
    { "REQ_STATE_R_ITEM",     state_read_item },

    { "REQ_STATE_ERROR",      state_error },
    { "REQ_STATE_DONE",		  state_done },
};

#define servo_session_states_size (sizeof(servo_session_states) \
    / sizeof(servo_session_states[0]))

const char *
servo_state(int s) 
{
    return servo_session_states[s].name;
}

const char *
servo_sql_state(int s)
{   
    return SQL_STATE_NAMES[s];
}

const char *
servo_request_state(struct http_request * req)
{
    return servo_state(req->fsm_state);
}

struct servo_context *
servo_create_context(struct http_request *req)
{
    struct servo_context *ctx;

    ctx = kore_malloc(sizeof(struct servo_context));
    memset(ctx->session.client, 0, sizeof(ctx->session.client));
    memset(&ctx->sql, 0, sizeof(struct kore_pgsql));
    
    ctx->status = 200;
    ctx->err = NULL;
    ctx->session.expire_on = 0;
    ctx->val_sz = 0;
    ctx->val_str = NULL;
    ctx->val_json = NULL;
    ctx->val_blob = NULL;

    /* read and write strings by default */
    ctx->in_content_type = SERVO_CONTENT_STRING;
    ctx->out_content_type = SERVO_CONTENT_STRING;

    return ctx;
}

void
servo_clear_context(struct servo_context *ctx)
{
    if (ctx->err != NULL)
        kore_free(ctx->err);
    if (ctx->val_str != NULL)
        kore_free(ctx->val_str);
    if (ctx->val_json != NULL)
        json_decref(ctx->val_json);
    if (ctx->val_blob != NULL)
        kore_free(ctx->val_blob);
}

int
servo_is_success(struct servo_context *ctx)
{
    if (ctx->status >= 200 && ctx->status < 300) {
        return 1;
    }

    return 0;
}

int
servo_is_redirect(struct servo_context *ctx)
{
    if (ctx->status >= 300 && ctx->status < 400)
        return 1;

    return 0;
}

int
servo_init(int state)
{
    CONFIG = kore_malloc(sizeof(struct servo_config));
    memset(CONFIG, 0, sizeof(struct servo_config));

    /* Configuration defaults */
    CONFIG->public_mode = 0;
    CONFIG->session_ttl = 300;
    CONFIG->max_sessions = 10;
    CONFIG->string_size = 255;
    CONFIG->json_size = 1024;
    CONFIG->blob_size = 4096;
    CONFIG->allow_origin = NULL;
    CONFIG->allow_ipaddr = NULL;

    if (!servo_read_config(CONFIG)) {
        kore_log(LOG_ERR, "%s: servo is not configured", __FUNCTION__);
        return (KORE_RESULT_ERROR);
    }

    kore_log(LOG_NOTICE, "started worker pid: %d", (int)getpid());
    kore_log(LOG_NOTICE, "  public mode: %s", CONFIG->public_mode != 0 ? "yes" : "no");
    kore_log(LOG_NOTICE, "  session ttl: %zu seconds", CONFIG->session_ttl);
    kore_log(LOG_NOTICE, "  max sessions: %zu", CONFIG->max_sessions);
    if (CONFIG->allow_origin != NULL)
        kore_log(LOG_NOTICE, "  allow origin: %s", CONFIG->allow_origin);
    if (CONFIG->allow_ipaddr != NULL)
        kore_log(LOG_NOTICE, "  allow ip address: %s", CONFIG->allow_ipaddr);
    
    kore_pgsql_register(DBNAME, CONFIG->database);
    
    return (KORE_RESULT_OK);
}


/**
 * Servo Session API entry point
 */
int servo_start(struct http_request *req)
{
    char                    *usrclient = NULL, *origin = NULL;
    char                     saddr[INET6_ADDRSTRLEN];
    uuid_t                   cid;
    struct servo_context    *ctx;

    if (req->hdlr_extra == NULL) {
       req->hdlr_extra = servo_create_context(req);
    }
    ctx = req->hdlr_extra;

    /* Filter by Origin header */
    if (servo_is_item_request(req) && CONFIG->allow_origin != NULL) {
        if (!http_request_header(req, "Origin", &origin) && !CONFIG->public_mode) {
            kore_log(LOG_NOTICE, "%s: disallow access - no 'Origin' header sent",
                __FUNCTION__);
            servo_response_status(req, 403, "'Origin' header is not found");
            return (KORE_RESULT_OK);
        }
        if (strcmp(origin, CONFIG->allow_origin) != 0) {
            kore_log(LOG_NOTICE, "%s: disallow access - 'Origin' header mismatch %s != %s",
                __FUNCTION__,
                origin, CONFIG->allow_origin);
            servo_response_status(req, 403, "Origin Access Denied");
            return (KORE_RESULT_OK);
        }
    }

    /* Filter by client ip address */
    if (CONFIG->allow_ipaddr != NULL) {
        memset(saddr, 0, sizeof(saddr));
        if (req->owner->addrtype == AF_INET) {
            inet_ntop(AF_INET, &req->owner->addr.ipv4.sin_addr, saddr, sizeof(saddr));
        }
        if (req->owner->addrtype == AF_INET6) {
            inet_ntop(AF_INET6, &req->owner->addr.ipv6.sin6_addr, saddr, sizeof(saddr));
        }

        if (strcmp(saddr, CONFIG->allow_ipaddr) != 0) {
            kore_log(LOG_NOTICE, "%s: disallow access - Client IP mismatch %s != %s",
                __FUNCTION__, saddr, CONFIG->allow_ipaddr);
            servo_response_status(req, 403, "Client Access Denied");
            return (KORE_RESULT_OK);
        }
    }

    if (strlen(ctx->session.client) == 0) {
        /* Read client ID from header and cookie */
        if (http_request_header(req, "X-Servo-Client", &usrclient)) {
            kore_strlcpy(ctx->session.client, usrclient, sizeof(ctx->session.client));
        }
        if (usrclient == NULL) {
            http_populate_cookies(req);
            if (http_request_cookie(req, "Servo-Client", &usrclient)) {
                kore_strlcpy(ctx->session.client, usrclient, sizeof(ctx->session.client));
            }
        }
        if (usrclient == NULL) {
            /* Generate new client id and init fresh session */
            uuid_generate(cid);
            memset(ctx->session.client, 0, sizeof(ctx->session.client));
            uuid_unparse(cid, ctx->session.client);
            kore_log(LOG_DEBUG, "new client without identifier, generated {%s}",
                ctx->session.client);
        }

        /* Pass generated ID to client */        
        http_response_header(req, "X-Servo-Client", ctx->session.client);
        http_response_cookie(req, "Servo-Client", ctx->session.client,
            HTTP_COOKIE_SECURE | HTTP_COOKIE_HTTPONLY);
    }

    return (http_state_run(servo_session_states, servo_session_states_size, req));
}

int
servo_connect_db(struct http_request *req, int retry_step, int success_step, int error_step)
{
    struct servo_context    *ctx = req->hdlr_extra;
    
    kore_pgsql_cleanup(&ctx->sql);
    if (!kore_pgsql_query_init(&ctx->sql, req, DBNAME, KORE_PGSQL_ASYNC)) {

        /* If the state was still INIT, we'll try again later. */
        if (ctx->sql.state == KORE_PGSQL_STATE_INIT) {
            req->fsm_state = retry_step;
            kore_log(LOG_ERR, "retrying connection, sql state is '%s'",
                servo_sql_state(ctx->sql.state));
            return (HTTP_STATE_RETRY);
        }

        /* Different state means error */
        kore_pgsql_logerror(&ctx->sql);
        ctx->status = 500;
        req->fsm_state = error_step;
        kore_log(LOG_ERR, "%s: failed to connect to database, sql state is '%s'",
            __FUNCTION__,
            servo_sql_state(ctx->sql.state));
        kore_log(LOG_NOTICE,
            "hint: check database connection string in the configuration file.");
    }
    else {
        req->fsm_state = success_step;
    }

    return (HTTP_STATE_CONTINUE);
}

void 
servo_handle_pg_error(struct http_request *req)
{
    struct servo_context *ctx = req->hdlr_extra;

    // default failure code
    ctx->status = 500;

    if (strstr(ctx->sql.error, "duplicate key value violates unique constraint") != NULL) {
        ctx->status = 409; // Conflict
    }

    if (ctx->err == NULL) {
        ctx->err = kore_strdup(ctx->sql.error);
    }
}

int
servo_wait(struct http_request *req, int read_step, int complete_step, int error_step)
{
    struct servo_context *ctx = req->hdlr_extra;

    switch (ctx->sql.state) {
    case KORE_PGSQL_STATE_WAIT:
        /* keep waiting */
        kore_log(LOG_DEBUG, "io wating ~> %s", servo_request_state(req));
        return (HTTP_STATE_RETRY);

    case KORE_PGSQL_STATE_COMPLETE:
        req->fsm_state = complete_step;
        kore_log(LOG_DEBUG, "io complete ~> %s", servo_request_state(req));
        break;

    case KORE_PGSQL_STATE_RESULT:
        req->fsm_state = read_step;
        kore_log(LOG_DEBUG, "io reading ~> %s", servo_request_state(req));
        break;

    case KORE_PGSQL_STATE_ERROR:
        req->fsm_state = error_step;
        kore_log(LOG_ERR, "io failed ~> %s.\n%s",
            servo_request_state(req),
            ctx->sql.error);
        servo_handle_pg_error(req);
        break;

    default:
        kore_pgsql_continue(req, &ctx->sql);
        break;
    }

    return (HTTP_STATE_CONTINUE);
}


/* An error occurred. */
int state_error(struct http_request *req)
{
    struct servo_context    *ctx = req->hdlr_extra;
    const char              *msg;

    kore_pgsql_cleanup(&ctx->sql);

    /* Handle redirect */
    if (servo_is_redirect(ctx)) {
        msg = http_status_text(ctx->status);
        kore_log(LOG_DEBUG, "%d: %s ~> '%s' to {%s}", 
            ctx->status,
            msg,
            req->path,
            ctx->session.client);

        http_response(req, ctx->status, msg, sizeof(msg));
        servo_clear_context(ctx);
        return (HTTP_STATE_COMPLETE);
    }

    if (servo_is_success(ctx)) {
        ctx->status = 500;
        kore_log(LOG_DEBUG, "no error status set, default=500");
    }

    kore_log(LOG_ERR, "%d: %s, sql state: %s to {%s}", 
        ctx->status, 
        http_status_text(ctx->status), 
        servo_sql_state(ctx->sql.state),
        ctx->session.client);
    servo_response_status(req, ctx->status, 
        ctx->err != NULL ? ctx->err : http_status_text(ctx->status));

    servo_clear_context(ctx);
    return (HTTP_STATE_COMPLETE);
}

/* Request was completed successfully. */
int state_done(struct http_request *req)
{
    struct servo_context    *ctx = req->hdlr_extra;
    const char                    *output;

    kore_pgsql_cleanup(&ctx->sql);

    if (req->method == HTTP_METHOD_POST ||
        req->method == HTTP_METHOD_PUT) 
    {
        switch(ctx->out_content_type) {
            case SERVO_CONTENT_STRING:
                http_response_header(req, "content-type", CONTENT_TYPE_STRING);
                break;
            case SERVO_CONTENT_JSON:
                http_response_header(req, "content-type", CONTENT_TYPE_JSON);
                break;

        }
        /* reply 201 Created on POSTs */
        if (req->method == HTTP_METHOD_POST)
            ctx->status = 201;

        output = http_status_text(ctx->status);
        switch(ctx->out_content_type) {
            default:
            case SERVO_CONTENT_BLOB:
                break;

            case SERVO_CONTENT_STRING:
                http_response(req, ctx->status,
                              output,
                              strlen(output));
                break;
            case SERVO_CONTENT_JSON:
                servo_response_status(req, ctx->status,
                                     output);
                break;
        }

    }
    else if (servo_is_item_request(req)) {

        kore_log(LOG_DEBUG, "serving item size %zu (%s) -> (%s) to {%s}",
            ctx->val_sz,
            SERVO_CONTENT_NAMES[ctx->in_content_type],
            SERVO_CONTENT_NAMES[ctx->out_content_type],
            ctx->session.client);
        
        switch(ctx->out_content_type) {
            default:
            case SERVO_CONTENT_STRING:
                output = servo_item_to_string(ctx);
                http_response_header(req, "content-type", CONTENT_TYPE_STRING);
                http_response(req, ctx->status, 
                              output == NULL ? "" : output,
                              output == NULL ? 0 : strlen(output));
                break;

            case SERVO_CONTENT_JSON:
                output = servo_item_to_json(ctx);
                http_response_header(req, "content-type", CONTENT_TYPE_JSON);
                http_response(req, ctx->status,
                              output == NULL ? "" : output,
                              output == NULL ? 0 : strlen(output));
                break;

            case SERVO_CONTENT_BLOB:
                servo_response_status(req, 403, http_status_text(403));
                break;

        };
    }
    else {
        ctx->status = 403;
        http_response(req, ctx->status, "", 0);
    }
    
    kore_log(LOG_DEBUG, "%d: %s to {%s}",
        ctx->status,
        http_status_text(ctx->status),
        ctx->session.client);

    servo_clear_context(ctx);
    return (HTTP_STATE_COMPLETE);
}

char *
servo_item_to_string(struct servo_context *ctx)
{
    char    *b64;

    switch(ctx->in_content_type) {
        case SERVO_CONTENT_STRING:
            return ctx->val_str;
        case SERVO_CONTENT_JSON:
            return json_dumps(ctx->val_json, JSON_INDENT(2));
        case SERVO_CONTENT_BLOB:
            kore_base64_encode(ctx->val_blob, ctx->val_sz, &b64);
            return b64;
    }

    return NULL;
}

char *
servo_item_to_json(struct servo_context *ctx)
{
    /* FIXME: apply json selectors here */
    return servo_item_to_string(ctx);
}