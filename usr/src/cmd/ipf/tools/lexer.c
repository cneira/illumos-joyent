/*
 * Copyright (C) 2002-2008 by Darren Reed.
 *
 * See the IPFILTER.LICENCE file for details on licencing.
 *
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2019 Joyent, Inc.
 */

#include <ctype.h>
#include "ipf.h"
#ifdef	IPFILTER_SCAN
#include "netinet/ip_scan.h"
#endif
#include <sys/ioctl.h>
#include <syslog.h>
#include <uuid/uuid.h>
#ifdef	TEST_LEXER
#define	NO_YACC
union	{
	int		num;
	char		*str;
	struct in_addr	ipa;
	i6addr_t	ip6;
	uuid_t		uuid;
} yylval;
#endif
#include "lexer.h"
#include "y.tab.h"

FILE *yyin;

#define	ishex(c)	(ISDIGIT(c) || ((c) >= 'a' && (c) <= 'f') || \
			((c) >= 'A' && (c) <= 'F'))
#define	TOOLONG		-3

extern int	string_start;
extern int	string_end;
extern char	*string_val;
extern int	pos;
extern int	yydebug;

char		*yystr = NULL;
int		yytext[YYBUFSIZ+1];
char		yychars[YYBUFSIZ+1];
int		yylineNum = 1;
int		yypos = 0;
int		yylast = -1;
int		yyexpectaddr = 0;
int		yybreakondot = 0;
int		yyvarnext = 0;
int		yytokentype = 0;
wordtab_t	*yywordtab = NULL;
int		yysavedepth = 0;
wordtab_t	*yysavewords[30];


static	wordtab_t	*yyfindkey __P((char *));
static	int		yygetc __P((int));
static	void		yyunputc __P((int));
static	int		yyswallow __P((int));
static	char		*yytexttostr __P((int, int));
static	void		yystrtotext __P((char *));
static	char		*yytexttochar __P((void));

static int yygetc(int docont)
{
	int c;

	if (yypos < yylast) {
		c = yytext[yypos++];
		if (c == '\n')
			yylineNum++;
		return (c);
	}

	if (yypos == YYBUFSIZ)
		return (TOOLONG);

	if (pos >= string_start && pos <= string_end) {
		c = string_val[pos - string_start];
		yypos++;
	} else {
		c = fgetc(yyin);
		if (docont && (c == '\\')) {
			c = fgetc(yyin);
			if (c == '\n') {
				yylineNum++;
				c = fgetc(yyin);
			}
		}
	}
	if (c == '\n')
		yylineNum++;
	yytext[yypos++] = c;
	yylast = yypos;
	yytext[yypos] = '\0';

	return (c);
}


static void yyunputc(int c)
{
	if (c == '\n')
		yylineNum--;
	yytext[--yypos] = c;
}


static int yyswallow(int last)
{
	int c;

	while (((c = yygetc(0)) > '\0') && (c != last))
		;

	if (c != EOF)
		yyunputc(c);
	if (c == last)
		return (0);
	return (-1);
}


static char *yytexttochar(void)
{
	int i;

	for (i = 0; i < yypos; i++)
		yychars[i] = (char)(yytext[i] & 0xff);
	yychars[i] = '\0';
	return (yychars);
}


static void yystrtotext(char *str)
{
	int len;
	char *s;

	len = strlen(str);
	if (len > YYBUFSIZ)
		len = YYBUFSIZ;

	for (s = str; *s != '\0' && len > 0; s++, len--)
		yytext[yylast++] = *s;
	yytext[yylast] = '\0';
}


static char *yytexttostr(int offset, int max)
{
	char *str;
	int i;

	if ((yytext[offset] == '\'' || yytext[offset] == '"') &&
	    (yytext[offset] == yytext[offset + max - 1])) {
		offset++;
		max--;
	}

	if (max > yylast)
		max = yylast;
	str = malloc(max + 1);
	if (str != NULL) {
		for (i = offset; i < max; i++)
			str[i - offset] = (char)(yytext[i] & 0xff);
		str[i - offset] = '\0';
	}
	return (str);
}


int
yylex(void)
{
	int c, n, isbuilding, rval, lnext, nokey = 0;
	char *name;

	isbuilding = 0;
	lnext = 0;
	rval = 0;

	if (yystr != NULL) {
		free(yystr);
		yystr = NULL;
	}

nextchar:
	c = yygetc(0);
	if (yydebug > 1)
		printf("yygetc = (%x) %c [%*.*s]\n", c, c, yypos, yypos,
		    yytexttochar());

	switch (c) {
	case '\n' :
		lnext = 0;
		nokey = 0;
		/* FALLTHROUGH */
	case '\t' :
	case '\r' :
	case ' ' :
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		if (yylast > yypos) {
			bcopy(yytext + yypos, yytext,
			    sizeof (yytext[0]) * (yylast - yypos + 1));
		}
		yylast -= yypos;
		yypos = 0;
		lnext = 0;
		nokey = 0;
		goto nextchar;

	case '\\' :
		if (lnext == 0) {
			lnext = 1;
			if (yylast == yypos) {
				yylast--;
				yypos--;
			} else
				yypos--;
			if (yypos == 0)
				nokey = 1;
			goto nextchar;
		}
		break;
	}

	if (lnext == 1) {
		lnext = 0;
		if ((isbuilding == 0) && !ISALNUM(c)) {
			return (c);
		}
		goto nextchar;
	}

	switch (c) {
	case '#' :
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		yyswallow('\n');
		rval = YY_COMMENT;
		goto done;

	case '$' :
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		n = yygetc(0);
		if (n == '{') {
			if (yyswallow('}') == -1) {
				rval = -2;
				goto done;
			}
			(void) yygetc(0);
		} else {
			if (!ISALPHA(n)) {
				yyunputc(n);
				break;
			}
			do {
				n = yygetc(1);
			} while (ISALPHA(n) || ISDIGIT(n) || n == '_');
			yyunputc(n);
		}

		name = yytexttostr(1, yypos);		/* skip $ */

		if (name != NULL) {
			string_val = get_variable(name, NULL, yylineNum);
			free(name);
			if (string_val != NULL) {
				name = yytexttostr(yypos, yylast);
				if (name != NULL) {
					yypos = 0;
					yylast = 0;
					yystrtotext(string_val);
					yystrtotext(name);
					free(string_val);
					free(name);
					goto nextchar;
				}
				free(string_val);
			}
		}
		break;

	case '\'':
	case '"' :
		if (isbuilding == 1) {
			goto done;
		}
		do {
			n = yygetc(1);
			if (n == EOF || n == TOOLONG) {
				rval = -2;
				goto done;
			}
			if (n == '\n') {
				yyunputc(' ');
				yypos++;
			}
		} while (n != c);
		rval = YY_STR;
		goto done;
		/* NOTREACHED */

	case EOF :
		yylineNum = 1;
		yypos = 0;
		yylast = -1;
		yyexpectaddr = 0;
		yybreakondot = 0;
		yyvarnext = 0;
		yytokentype = 0;
		return (0);
	}

	if (strchr("=,/;{}()@", c) != NULL) {
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		rval = c;
		goto done;
	} else if (c == '.') {
		if (isbuilding == 0) {
			rval = c;
			goto done;
		}
		if (yybreakondot != 0) {
			yyunputc(c);
			goto done;
		}
	}

	switch (c) {
	case '-' :
		if (yyexpectaddr)
			break;
		if (isbuilding == 1)
			break;
		n = yygetc(0);
		if (n == '>') {
			isbuilding = 1;
			goto done;
		}
		yyunputc(n);
		rval = '-';
		goto done;

	case '!' :
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		n = yygetc(0);
		if (n == '=') {
			rval = YY_CMP_NE;
			goto done;
		}
		yyunputc(n);
		rval = '!';
		goto done;

	case '<' :
		if (yyexpectaddr)
			break;
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		n = yygetc(0);
		if (n == '=') {
			rval = YY_CMP_LE;
			goto done;
		}
		if (n == '>') {
			rval = YY_RANGE_OUT;
			goto done;
		}
		yyunputc(n);
		rval = YY_CMP_LT;
		goto done;

	case '>' :
		if (yyexpectaddr)
			break;
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		n = yygetc(0);
		if (n == '=') {
			rval = YY_CMP_GE;
			goto done;
		}
		if (n == '<') {
			rval = YY_RANGE_IN;
			goto done;
		}
		yyunputc(n);
		rval = YY_CMP_GT;
		goto done;
	}

	/*
	 * Now for the reason this is here...IPv6 address parsing.
	 * The longest string we can expect is of this form:
	 * 0000:0000:0000:0000:0000:0000:000.000.000.000
	 * not:
	 * 0000:0000:0000:0000:0000:0000:0000:0000
	 */
#ifdef	USE_INET6
	if (isbuilding == 0 && (ishex(c) || c == ':')) {
		char ipv6buf[45 + 1], *s, oc;
		int start;

		start = yypos;
		s = ipv6buf;
		oc = c;

		/*
		 * Perhaps we should implement stricter controls on what we
		 * swallow up here, but surely it would just be duplicating
		 * the code in inet_pton() anyway.
		 */
		do {
			*s++ = c;
			c = yygetc(1);
		} while ((ishex(c) || c == ':' || c == '.') &&
		    (s - ipv6buf < 46));
		yyunputc(c);
		*s = '\0';

		if (inet_pton(AF_INET6, ipv6buf, &yylval.ip6) == 1) {
			rval = YY_IPV6;
			yyexpectaddr = 0;
			goto done;
		}
		yypos = start;
		c = oc;
	}
#endif

	/*
	 * UUID: e.g., "2426e38c-9f63-c0b8-cfd5-9aaeaf992d42" or its uppercase
	 * variant.
	 */
	if (isbuilding == 0 && (ishex(c) || c == '-')) {
		char uuidbuf[UUID_PRINTABLE_STRING_LENGTH], *s, oc;
		int start;

		start = yypos;
		s = uuidbuf;
		oc = c;

		/*
		 * Don't worry about exact position of hexdigits and hyphens
		 * because uuid_parse() will provide the sanity check.
		 */
		do {
			*s++ = c;
			c = yygetc(1);
		} while ((ishex(c) || c == '-') &&
		    (s - uuidbuf < sizeof (uuidbuf)));
		yyunputc(c);
		*s = '\0';

		if (uuid_parse(uuidbuf, yylval.uuid) == 0) {
			rval = YY_UUID;
			yyexpectaddr = 0;
			goto done;
		}
		yypos = start;
		c = oc;
	}


	if (c == ':') {
		if (isbuilding == 1) {
			yyunputc(c);
			goto done;
		}
		rval = ':';
		goto done;
	}

	if (isbuilding == 0 && c == '0') {
		n = yygetc(0);
		if (n == 'x') {
			do {
				n = yygetc(1);
			} while (ishex(n));
			yyunputc(n);
			rval = YY_HEX;
			goto done;
		}
		yyunputc(n);
	}

	/*
	 * No negative numbers with leading - sign..
	 */
	if (isbuilding == 0 && ISDIGIT(c)) {
		do {
			n = yygetc(1);
		} while (ISDIGIT(n));
		yyunputc(n);
		rval = YY_NUMBER;
		goto done;
	}

	isbuilding = 1;
	goto nextchar;

done:
	yystr = yytexttostr(0, yypos);

	if (yydebug)
		printf("isbuilding %d yyvarnext %d nokey %d\n",
		    isbuilding, yyvarnext, nokey);
	if (isbuilding == 1) {
		wordtab_t *w;

		w = NULL;
		isbuilding = 0;

		if ((yyvarnext == 0) && (nokey == 0)) {
			w = yyfindkey(yystr);
			if (w == NULL && yywordtab != NULL) {
				yyresetdict();
				w = yyfindkey(yystr);
			}
		} else
			yyvarnext = 0;
		if (w != NULL)
			rval = w->w_value;
		else
			rval = YY_STR;
	}

	if (rval == YY_STR && yysavedepth > 0)
		yyresetdict();

	yytokentype = rval;

	if (yydebug)
		printf("lexed(%s) [%d,%d,%d] => %d @%d\n", yystr, string_start,
		    string_end, pos, rval, yysavedepth);

	switch (rval) {
	case YY_NUMBER :
		sscanf(yystr, "%u", &yylval.num);
		break;

	case YY_HEX :
		sscanf(yystr, "0x%x", (uint_t *)&yylval.num);
		break;

	case YY_STR :
		yylval.str = strdup(yystr);
		break;

	default :
		break;
	}

	if (yylast > 0) {
		bcopy(yytext + yypos, yytext,
		    sizeof (yytext[0]) * (yylast - yypos + 1));
		yylast -= yypos;
		yypos = 0;
	}

	return (rval);
}


static wordtab_t *yyfindkey(char *key)
{
	wordtab_t *w;

	if (yywordtab == NULL)
		return (NULL);

	for (w = yywordtab; w->w_word != 0; w++)
		if (strcasecmp(key, w->w_word) == 0)
			return (w);
	return (NULL);
}


char *
yykeytostr(int num)
{
	wordtab_t *w;

	if (yywordtab == NULL)
		return ("<unknown>");

	for (w = yywordtab; w->w_word; w++)
		if (w->w_value == num)
			return (w->w_word);
	return ("<unknown>");
}


wordtab_t *
yysettab(wordtab_t *words)
{
	wordtab_t *save;

	save = yywordtab;
	yywordtab = words;
	return (save);
}


int
yyerror(const char *msg)
{
	char *txt, letter[2];
	int freetxt = 0;

	if (yytokentype < 256) {
		letter[0] = yytokentype;
		letter[1] = '\0';
		txt =  letter;
	} else if (yytokentype == YY_STR || yytokentype == YY_HEX ||
	    yytokentype == YY_NUMBER) {
		if (yystr == NULL) {
			txt = yytexttostr(yypos, YYBUFSIZ);
			if (txt == NULL) {
				fprintf(stderr, "sorry, out of memory,"
				    " bailing out\n");
				exit(1);
			}
			freetxt = 1;
		} else
			txt = yystr;
	} else {
		txt = yykeytostr(yytokentype);
		if (txt == NULL) {
			fprintf(stderr, "sorry, out of memory,"
			    " bailing out\n");
			exit(1);
		}
	}
	fprintf(stderr, "%s error at \"%s\", line %d\n", msg, txt, yylineNum);
	if (freetxt == 1)
		free(txt);
	exit(1);
}


void
yysetdict(wordtab_t *newdict)
{
	if (yysavedepth == sizeof (yysavewords) / sizeof (yysavewords[0])) {
		fprintf(stderr, "%d: at maximum dictionary depth\n",
		    yylineNum);
		return;
	}

	yysavewords[yysavedepth++] = yysettab(newdict);
	if (yydebug)
		printf("yysavedepth++ => %d\n", yysavedepth);
}

void
yyresetdict(void)
{
	if (yydebug)
		printf("yyresetdict(%d)\n", yysavedepth);
	if (yysavedepth > 0) {
		yysettab(yysavewords[--yysavedepth]);
		if (yydebug)
			printf("yysavedepth-- => %d\n", yysavedepth);
	}
}



#ifdef	TEST_LEXER
int
main(int argc, char *argv[])
{
	int n;

	yyin = stdin;

	while ((n = yylex()) != 0)
		printf("%d.n = %d [%s] %d %d\n",
		    yylineNum, n, yystr, yypos, yylast);
}
#endif
