#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <amqp_ssl_socket.h>
#include "stats.h"
#include "stats_buffer.h"
#include "schema.h"
#include "trace.h"
#include "pscanf.h"
#include "string1.h"

#define SF_SCHEMA_CHAR '!'
#define SF_DEVICES_CHAR '@'
#define SF_COMMENT_CHAR '#'
#define SF_PROPERTY_CHAR '$'
#define SF_MARK_CHAR '%'


#define sf_printf(sf, fmt, args...) do {			\
    char *tmp_string = sf->sf_data;				\
    asprintf(&(sf->sf_data), "%s"fmt, sf->sf_data, ##args);	\
    free(tmp_string);						\
  } while(0)

static int send(struct stats_buffer *sf)
{
  int status = -1;
  char const *exchange;
  amqp_socket_t *socket = NULL;
  amqp_connection_state_t conn;

  exchange = "stats-listener";
  conn = amqp_new_connection();
  socket = amqp_ssl_socket_new(conn);

  amqp_ssl_socket_set_verify_peer(socket, 1);
  amqp_ssl_socket_set_verify_hostname(socket, 1);
  //http://stackoverflow.com/questions/22660940/rabbitmq-c-client-ssl-errors
  amqp_ssl_socket_set_cacert(socket, "/var/secrets/cometvc/ca.pem");
  amqp_ssl_socket_set_key(socket, "/var/secrets/cometvc/cert.pem", "/var/secrets/cometvc/key.pem");
  
  if (!socket) {
    ERROR("socket failed to initialize");
    return -1;	
  }

  status = amqp_socket_open(socket, sf->sf_host, atoi(sf->sf_port));
  if (status) {
    ERROR("socket failed to open");
    return -1;	  
  }

  // amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, 
	 //     "guest", "guest");
  amqp_login(conn, "xsede_stats", 0, 131072, 0, AMQP_SASL_METHOD_EXTERNAL, "comet-stats.sdsc.edu");
  amqp_channel_open(conn, 1);
  amqp_get_rpc_reply(conn);

  if (!queue_declared) {
    syslog(LOG_INFO, "Attempt declare queue on RMQ server\n");
    amqp_queue_declare_ok_t *r = amqp_queue_declare(conn, 1, amqp_cstring_bytes(sf->sf_queue), 
						    0, 1, 0, 0, amqp_empty_table);
    amqp_rpc_reply_t ret = amqp_get_rpc_reply(conn);
    if (ret.reply_type != AMQP_RESPONSE_NORMAL) {
      syslog(LOG_ERR, "queue declare failed");
      return -1;
    }
    else {
      amqp_bytes_t reply_to_queue;
      reply_to_queue = amqp_bytes_malloc_dup(r->queue);
      if (reply_to_queue.bytes == NULL) {
        syslog(LOG_ERR, "Out of memory while copying queue name");
        return -1;
      }
      
      amqp_queue_bind(conn, 1, reply_to_queue, amqp_cstring_bytes(exchange), 
		      amqp_cstring_bytes(sf->sf_queue), amqp_empty_table);
      amqp_get_rpc_reply(conn);
      queue_declared = 1;
      amqp_bytes_free(reply_to_queue);
    }
  }
  struct utsname uts_buf;
  uname(&uts_buf);

  {
    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG | AMQP_BASIC_REPLY_TO_FLAG | AMQP_BASIC_TYPE_FLAG;
    props.content_type = amqp_cstring_bytes("text/plain");
    props.delivery_mode = 2; /* persistent delivery mode */
    props.reply_to = amqp_cstring_bytes(uts_buf.nodename);
    props.type = amqp_cstring_bytes("stat");
    amqp_basic_publish(conn,
		       1,
		       amqp_cstring_bytes(exchange),
		       amqp_cstring_bytes(""),
		       0,
		       0,
		       &props,
		       amqp_cstring_bytes(sf->sf_data));
  }

  amqp_destroy_connection(conn); 

  return 0;
}

int stats_wr_hdr(struct stats_buffer *sf)
{
  struct utsname uts_buf;
  unsigned long long uptime = 0;
  
  uname(&uts_buf);
  pscanf("/proc/uptime", "%llu", &uptime);
  
  sf_printf(sf, "%c%s %s\n", SF_PROPERTY_CHAR, STATS_PROGRAM, STATS_VERSION);
  sf_printf(sf, "%chostname %s\n", SF_PROPERTY_CHAR, uts_buf.nodename);
  sf_printf(sf, "%cuname %s %s %s %s\n", SF_PROPERTY_CHAR, uts_buf.sysname,
            uts_buf.machine, uts_buf.release, uts_buf.version);
  sf_printf(sf, "%cuptime %llu\n", SF_PROPERTY_CHAR, uptime);
  
  size_t i = 0;
  struct stats_type *type;
  while ((type = stats_type_for_each(&i)) != NULL) {
    if (!type->st_enabled)
      continue;

    TRACE("type %s, schema_len %zu\n", type->st_name, type->st_schema.sc_len);

    /* Write schema. */
    sf_printf(sf, "%c%s", SF_SCHEMA_CHAR, type->st_name);

    /* MOVEME */
    size_t j;
    for (j = 0; j < type->st_schema.sc_len; j++) {
      struct schema_entry *se = type->st_schema.sc_ent[j];
      sf_printf(sf, " %s", se->se_key);
      if (se->se_type == SE_CONTROL)
        sf_printf(sf, ",C");
      if (se->se_type == SE_EVENT)
        sf_printf(sf, ",E");
      if (se->se_unit != NULL)
        sf_printf(sf, ",U=%s", se->se_unit);
      if (se->se_width != 0)
        sf_printf(sf, ",W=%u", se->se_width);
    }
    sf_printf(sf, "\n");
  }

  return 0;
}

int stats_buffer_open(struct stats_buffer *sf, const char *host, const char *port, const char *queue)
{
  int rc = 0;
  memset(sf, 0, sizeof(*sf));
  sf->sf_data=strdup("");
  sf->sf_host=strdup(host);
  sf->sf_port=strdup(port);
  sf->sf_queue=strdup(queue);

  return rc;
}

int stats_buffer_close(struct stats_buffer *sf)
{
  int rc = 0;
  
  free(sf->sf_data);
  free(sf->sf_host);
  free(sf->sf_port);
  free(sf->sf_queue);
  free(sf->sf_mark);
  memset(sf, 0, sizeof(*sf));
  return rc;
}

int stats_buffer_mark(struct stats_buffer *sf, const char *fmt, ...)
{
  /* TODO Concatenate new mark with old. */
  va_list args;
  va_start(args, fmt);

  if (vasprintf(&sf->sf_mark, fmt, args) < 0)
    sf->sf_mark = NULL;

  va_end(args);
  return 0;
}

int stats_buffer_write(struct stats_buffer *sf)
{
  int rc = 0;

  struct utsname uts_buf;
  uname(&uts_buf);

  sf_printf(sf, "\n%f %s %s\n", current_time, current_jobid, uts_buf.nodename);

  /* Write mark. */
  if (sf->sf_mark != NULL) {
    const char *str = sf->sf_mark;
    while (*str != 0) {
      const char *eol = strchrnul(str, '\n');
      sf_printf(sf, "%c%*s\n", SF_MARK_CHAR, (int) (eol - str), str);
      str = eol;
      if (*str == '\n')
        str++;
    }
  }

  /* Write stats. */
  size_t i = 0;
  struct stats_type *type;
  while ((type = stats_type_for_each(&i)) != NULL) {
    if (!(type->st_enabled))
      continue;

    size_t j = 0;
    char *dev;
    while ((dev = dict_for_each(&type->st_current_dict, &j)) != NULL) {
      struct stats *stats = key_to_stats(dev);

      sf_printf(sf, "%s %s", type->st_name, stats->s_dev);
      size_t k;
      for (k = 0; k < type->st_schema.sc_len; k++)
	{
	  sf_printf(sf, " %llu", stats->s_val[k]);
	}
      sf_printf(sf, "\n");
    }
  }

  rc = send(sf);
  return rc;
}
