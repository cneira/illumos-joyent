/*
 * Copyright 2019 Joyent, Inc.
 */

#ifndef LEXER_H
#define	LEXER_H

typedef	struct	wordtab	{
	char	*w_word;
	int	w_value;
} wordtab_t;

#ifdef	NO_YACC
#define	YY_COMMENT	1000
#define	YY_CMP_NE	1001
#define	YY_CMP_LE	1002
#define	YY_RANGE_OUT	1003
#define	YY_CMP_GE	1004
#define	YY_RANGE_IN	1005
#define	YY_HEX		1006
#define	YY_NUMBER	1007
#define	YY_IPV6		1008
#define	YY_STR		1009
#define	YY_IPADDR	1010
#define	YY_UUID		1011
#endif

#define	YYBUFSIZ	8192

extern	wordtab_t	*yysettab __P((wordtab_t *));
extern	void		yysetdict __P((wordtab_t *));
extern	int		yylex __P((void));
extern	int		yyerror __P((const char *));
extern	char		*yykeytostr __P((int));
extern	void		yyresetdict __P((void));

extern	FILE	*yyin;
extern	int	yylineNum;
extern	int	yyexpectaddr;
extern	int	yybreakondot;
extern	int	yyvarnext;

#endif /* LEXER_H */
