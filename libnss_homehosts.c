
#include <arpa/inet.h>
#include <nss.h>
#include <netdb.h>
#include <errno.h>
#include <err.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include "res_hconf.h"


struct ipaddr {
	int af;
	struct in_addr ip4;
	struct in6_addr ip6;
};

typedef int bool;
#define TRUE 1
#define FALSE 0

#define ALIGN(idx) do { \
  if (idx % sizeof(void*)) \
    idx += (sizeof(void*) - idx % sizeof(void*)); /* Align on 32 bit boundary */ \
} while(0)

#define AFLEN(af) (((af) == AF_INET6) ? sizeof(struct in6_addr) : sizeof(struct in_addr))


int parseIpStr(const char *str, struct ipaddr *addr)
{
	/* Convert string to IPv4/v6 address, or fail */
	/* Return: 1 on success */
	int ok;
	
	addr->af = AF_INET;
	ok = inet_pton(AF_INET, str, &(addr->ip4));
	if(ok == -1) perror("inet_pton");
	if(ok != 1)
	{
		addr->af = AF_INET6;
		ok = inet_pton(AF_INET6, str, &(addr->ip6));
		if(ok == -1) perror("inet_pton");
	}
	return ok;
}

void* ipaddr_get_binary_addr(struct ipaddr *addr)
{
	if(addr->af == AF_INET) return &(addr->ip4.s_addr);
	if(addr->af == AF_INET6) return &(addr->ip6.__in6_u);
	return NULL;
}

void seek_line(FILE* fh)
{
	/* Seeks to the beginning of next non-empty line on a file. */
	fscanf(fh, "%*[^\n]%*[\n]");
}

#ifdef DEBUG
void dumpbuffer(unsigned char* buf, size_t len)
{
	unsigned char* p = buf;
	while(p - buf < len)
	{
		fprintf(stderr, "%02X %c ", p[0], isprint(p[0])?p[0]:'.');
		if(((p - buf) % 16) == 15) fprintf(stderr, "\n");
		else if(((p - buf) % 8) == 7) fprintf(stderr, "| ");
		p++;
	}
}
#endif

enum nss_status homehosts_gethostent_r(
	const char *query_name,
	const void *query_addr,
	struct hostent * result,
	char *buffer,
	size_t buflen,
	struct hostent ** result_p,
	int *h_errnop,
	int query_af)
{
	size_t idx, ridx, addrstart;		// cursors in buffer space
	struct ipaddr address;
	FILE *fh;
	long aliases_offset;
	char homehosts_file[PATH_MAX+1];
	char ipbuf[INET6_ADDRSTRLEN];
	char namebuf[_POSIX_HOST_NAME_MAX+1];
	char nlbuf[2];		// holds a newline char
	char *c;
	int cnt, acnt, tokens;
	bool store_aliases_phase, ipaddr_found = FALSE;
	
	
	#ifdef DEBUG
	warnx("host.conf: inited = %u, flags = %u, multi = %u", _res_hconf.initialized, _res_hconf.flags, (_res_hconf.flags & HCONF_FLAG_MULTI)!=0);
	memset(buffer, ' ', buflen);
	#endif
	
	/* Open hosts file */
	
	cnt = snprintf(homehosts_file, PATH_MAX, "%s/.hosts", getenv("HOME"));
	if(cnt >= PATH_MAX) goto soft_error;
	fh = fopen(homehosts_file, "r");
	if(fh == NULL) goto soft_error;
	
	/* Copy requested name to canonical hostname */
	
	idx = 0;
	ridx = buflen;		// first byte occupied at the end of buffer
	result->h_name = NULL;
	if(query_name != NULL)
	{
		strcpy(buffer+idx, query_name);
		result->h_name = buffer+idx;
		idx += strlen(query_name)+1;
		ALIGN(idx);
	}
	addrstart = idx;
	
	result->h_addrtype = query_af;
	result->h_length = AFLEN(query_af);
	
	/* Read hosts file */
	
	cnt = 0;	// Count resulting addresses
	acnt = 0;	// Count resulting alias names
	while(!feof(fh))
	{
		if(fscanf(fh, "%s", (char*)&ipbuf) == 1)
		{
			if(ipbuf[0] == '#')
			{
				seek_line(fh);
				continue;
			}
			
			store_aliases_phase = FALSE;
			aliases_offset = ftell(fh);
			
			if(query_addr != NULL)
			{
				if(parseIpStr(ipbuf, &address) == 1 
				   && address.af == query_af 
				   && memcmp(ipaddr_get_binary_addr(&address), query_addr /* TODO: use struct members */, result->h_length)==0)
				{
					ipaddr_found = TRUE;
					store_aliases_phase = TRUE;
					cnt++;
					memcpy(buffer+idx, ipaddr_get_binary_addr(&address), result->h_length);
					idx += result->h_length;
				}
				else
				{
					seek_line(fh);
					continue;
				}
			}
			
			read_hostname:
			tokens = fscanf(fh, "%s%1[\n]", namebuf, nlbuf);
			if(tokens > 0)
			{
				#ifdef DEBUG
				warnx("alias phase %d, name '%s'", store_aliases_phase, namebuf);
				#endif
				c = strchr(namebuf, '#');
				if(c != NULL)
				{
					/* Strip comment */
					*c = '\0';
					/* Treat as we saw newline */
					tokens = 2;
					/* Seek to the next line */
					fscanf(fh, "%*[^\n]%*[\n]");
				}
				
				if(store_aliases_phase)
				{
					if(query_name == NULL || strcasecmp(namebuf, query_name)!=0)
					{
						if(result->h_name == NULL)
						{
							/* Save 1st hostname as canonical name */
							strcpy(buffer+idx, namebuf);
							result->h_name = buffer+idx;
							idx += strlen(namebuf)+1;
							ALIGN(idx);
						}
						else
						{
							acnt++;
							if(idx + strlen(namebuf)+1 /* trailing NUL byte */ + (acnt+1) * sizeof(char*) /* pointers to alias names */ > ridx-1)
							{
								fclose(fh);
								goto buffer_error;
							}
							strcpy(buffer+ridx-strlen(namebuf)-1, namebuf);
							ridx += -strlen(namebuf)-1;
						}
					}
				}
				else
				{
					if(strcasecmp(namebuf, query_name)==0)
					{
						/* hostname matches */
						if(parseIpStr(ipbuf, &address) == 1 && address.af == query_af)
						{
							/* hostname matches and ip address is valid */
							cnt++;
							if(idx + result->h_length + (cnt+1) * sizeof(char*) > ridx-1)
							{
								fclose(fh);
								goto buffer_error;
							}
							
							memcpy(buffer+idx, ipaddr_get_binary_addr(&address), result->h_length);
							idx += result->h_length;
							
							/* Treat other hostnames in this line as aliases */
							store_aliases_phase = TRUE;
							fseek(fh, aliases_offset, 0);
							goto read_hostname;
						}
					}
				}
			}
			
			if(tokens != 1)
			{
				/* Encountered a newline */
				if(cnt > 0 && (ipaddr_found || (_res_hconf.flags & HCONF_FLAG_MULTI)==0))
					/* Do not continue line reading,
					   because either address is found or
					   hostname is found and 'multi off' in host.conf */
					break;
				continue;
			}
			
			goto read_hostname;
		}
	}
	fclose(fh);	
	
	if(cnt == 0)
	{
		*h_errnop = NO_ADDRESS;
		return NSS_STATUS_NOTFOUND;
	}
	
	/* Store pointers to addresses */
	
	result->h_addr_list = (char**)(buffer + idx);
	int n = 0;
	for(; n < cnt; n++)
	{
		result->h_addr_list[n] = (char*)(buffer + addrstart + n * result->h_length);
	}
	result->h_addr_list[n] = NULL;
	idx += (n+1) * sizeof(char*);
	
	/* Store pointers to aliases */
	
	result->h_aliases = (char**)(buffer + idx);
	n = 0;
	for(; n < acnt; n++)
	{
		char* alias = (char*)(buffer + ridx);
		#ifdef DEBUG
		warnx("acnt %d, alias '%s'", acnt, alias);
		#endif
		result->h_aliases[n] = alias;
		ridx += strlen(alias) + 1;
	}
	result->h_aliases[n] = NULL;
	
	#ifdef DEBUG
	warnx("h_name -> %u\nh_aliases -> %u\nh_addrtype = %u\nh_length = %u\nh_addr_list -> %u", (void*)result->h_name - (void*)buffer, (void*)result->h_aliases - (void*)buffer, result->h_addrtype, result->h_length, (void*)result->h_addr_list - (void*)buffer);
	dumpbuffer(buffer, buflen);
	#endif
	
	*result_p = result;
	return NSS_STATUS_SUCCESS;
	
	
	buffer_error:
	*h_errnop = ERANGE;
	*result_p = NULL;
	return NSS_STATUS_TRYAGAIN;
	
	soft_error:
	*h_errnop = EAGAIN;
	*result_p = NULL;
	return NSS_STATUS_TRYAGAIN;
}

enum nss_status _nss_homehosts_gethostbyname_r(
	const char *name,
	struct hostent * result,
	char *buffer,
	size_t buflen,
	struct hostent ** result_p,
	int *h_errnop)
{
	enum nss_status found_ipv6;
	found_ipv6 = homehosts_gethostent_r(name, NULL, result, buffer, buflen, result_p, h_errnop, AF_INET6);
	if(found_ipv6 == NSS_STATUS_NOTFOUND)
		return homehosts_gethostent_r(name, NULL, result, buffer, buflen, result_p, h_errnop, AF_INET);
	return found_ipv6;
}

enum nss_status _nss_homehosts_gethostbyname2_r(
	const char *name,
	int af,
	struct hostent * result,
	char *buffer,
	size_t buflen,
	struct hostent ** result_p,
	int *h_errnop)
{
	if(af != AF_INET && af != AF_INET6)
	{
		*h_errnop = EAFNOSUPPORT;
		*result_p = NULL;
		return NSS_STATUS_UNAVAIL;
	}
	else
	{
		return homehosts_gethostent_r(name, NULL, result, buffer, buflen, result_p, h_errnop, af);
	}
}

enum nss_status _nss_homehosts_gethostbyaddr_r(
	const void *address,
	socklen_t len,
	int af,
	struct hostent * result,
	char *buffer,
	size_t buflen,
	struct hostent ** result_p,
	int *h_errnop)
{
	return homehosts_gethostent_r(NULL, address, result, buffer, buflen, result_p, h_errnop, af);
}