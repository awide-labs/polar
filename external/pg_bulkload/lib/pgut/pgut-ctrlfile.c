/*-------------------------------------------------------------------------
 *
 * pgut-ctrlfile.c
 *
 * Copyright (c) 2009-2026, NTT, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifdef FRONTEND
#include "../../bin/pgut/pgut.h"
#else
#include "postgres.h"
#include "pgut-be.h"
#endif
#include "pgut-ctrlfile.h"

#include <ctype.h>
#include <string.h>

static char *TrimSpaces(char *input);
static char *UnquoteString(char *str, char quote, char escape);
static char *FindUnquotedChar(char *str, char target, char quote, char escape);

/**
 * @brief Parse a line in control file.
 */
bool
ParseControlFileLine(char buf[], char **outKeyword, char **outValue)
{
	char	   *keyword = NULL;
	char	   *value = NULL;
	char	   *p;
	char	   *q;

	*outKeyword = NULL;
	*outValue = NULL;

	p = buf;					/* pointer to keyword */

	/*
	 * replace '\n' to '\0'
	 */
	q = strchr(buf, '\n');
	if (q != NULL)
		*q = '\0';

	/*
	 * delete strings after a comment letter outside quotations
	 */
	q = FindUnquotedChar(buf, '#', '"', '\\');
	if (q != NULL)
		*q = '\0';

	/*
	 * if result of trimming is a null string, it is treated as an empty line
	 */
	p = TrimSpaces(buf);
	if (*p == '\0')
		return false;

	/*
	 * devide after '='
	 */
	q = FindUnquotedChar(buf, '=', '"', '\\');
	if (q != NULL)
		*q = '\0';
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input \"%s\"", buf)));

	q++;						/* pointer to input value */

	/*
	 * return a value trimmed space
	 */
	keyword = TrimSpaces(p);
	value = TrimSpaces(q);

	if (!keyword[0] || !value[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input \"%s\"", buf)));

	value = UnquoteString(value, '"', '\\');
	if (!value)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unterminated quoted field")));

	*outKeyword = keyword;
	*outValue = value;
	return true;
}

/**
 * @brief Trim white spaces before and after input value.
 *
 * Flow
 * <ol>
 *	 <li>Trim spaces after input value. </li>
 *	 <li>Search the first non-space character, and return the pointer. </li>
 * </ol>
 * @param input		[in/out] Input character string
 * @return The pointer for the head of the character string after triming spaces
 * @note Input string is over written.
 * @note The returned value points the middle of input string.
 */
static char *
TrimSpaces(char *input)
{
	char	   *beg;
	char	   *end;

	/* trim spaces at head */
	for (beg = input; isspace((unsigned char) *beg); beg++)
		;

	/* trim spaces at tail */
	for (end = beg + strlen(beg); end > beg && isspace((unsigned char) end[-1]); end--)
		;
	*end = '\0';

	return beg;
}

/**
 * @brief Trim quotes surrounding string
 *
 * Quoting character(i.e. quote and escape character) is transformed as follows.
 * <ul>
 *	 <li>abc -> abc</li>
 *	 <li>"abc" -> abc</li>
 *	 <li>"abc\"123" -> abc"123</li>
 *	 <li>"abc\\123" -> abc\123</li>
 *	 <li>"abc\123" -> abc\123</li>
 *	 <li>"abc"123 -> abc123</li>
 *	 <li>"abc""123" -> abc123</li>
 *	 <li>"abc -> NG(error occuring) </li>
 * </ul>
 * @param str [in/out] Proccessed string
 * @param quote [in] Quote mark character
 * @param escape [in] Escape character
 * @retval !NULL String not surrounding quote mark character
 * @retval NULL  Error(not closed by quote mark)
 */
static char *
UnquoteString(char *str, char quote, char escape)
{
	int			i;				/* Read position */
	int			j;				/* Write position */
	int			in_quote = 0;


	for (i = 0, j = 0; str[i]; i++)
	{
		/*
		 * Find an opened quote mark.
		 */
		if (!in_quote && str[i] == quote)
		{
			in_quote = 1;
			continue;
		}

		/*
		 * Find an closing quote mark.
		 */
		if (in_quote && str[i] == quote)
		{
			in_quote = 0;
			continue;
		}

		/*
		 * Find an escape character.
		 * Process if the next is meta character.
		 */
		if (in_quote && str[i] == escape)
		{
			if (str[i + 1] == quote)
			{
				str[j++] = quote;
				i++;
				continue;
			}
			else if (str[i + 1] == escape)
			{
				str[j++] = escape;
				i++;
				continue;
			}
		}

		/*
		 * If it is ordinal character, copy it without modification.
		 */
		str[j++] = str[i];
	}
	str[j] = '\0';

	/*
	 * Quote mark is not closed
	 */
	if (in_quote)
		return NULL;

	return str;
}

/**
 * @brief Find the first specified character outside of quote mark
 * @param str [in] Searched string
 * @param target [in] Searched character
 * @param quote [in] Quote mark
 * @param escape [in] Escape character
 * @return If the specified character is found outside quoted string, return the
 * pointer. If it is not found, return NULL.
 */
static char *
FindUnquotedChar(char *str, char target, char quote, char escape)
{
	int			i;
	bool		in_quote = false;

	for (i = 0; str[i]; i++)
	{
		if (str[i] == escape)
		{
			/*
			 * Treat it as escape character if it is before meta character
			 */
			if (str[i + 1] == escape || str[i + 1] == quote)
				i++;
		}
		else if (str[i] == quote)
			in_quote = !in_quote;
		else if (!in_quote && str[i] == target)
			return str + i;
	}

	return NULL;
}
