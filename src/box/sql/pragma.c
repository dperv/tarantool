/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the PRAGMA command.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "box/session.h"

#if !defined(SQLITE_ENABLE_LOCKING_STYLE)
#if defined(__APPLE__)
#define SQLITE_ENABLE_LOCKING_STYLE 1
#else
#define SQLITE_ENABLE_LOCKING_STYLE 0
#endif
#endif

/*
*************************************************************************
** pragma.h contains several pragmas, including utf's pragmas.
** All that is not utf-8 should be omitted
*************************************************************************
*/

/***************************************************************************
** The "pragma.h" include file is an automatically generated file that
** that includes the PragType_XXXX macro definitions and the aPragmaName[]
** object.  This ensures that the aPragmaName[] table is arranged in
** lexicographical order to facility a binary search of the pragma name.
** Do not edit pragma.h directly.  Edit and rerun the script in at
** ../tool/mkpragmatab.tcl. */
#include "pragma.h"

/*
** Interpret the given string as a safety level.  Return 0 for OFF,
** 1 for ON or NORMAL, 2 for FULL, and 3 for EXTRA.  Return 1 for an empty or
** unrecognized string argument.  The FULL and EXTRA option is disallowed
** if the omitFull parameter it 1.
**
** Note that the values returned are one less that the values that
** should be passed into sqlite3BtreeSetSafetyLevel().  The is done
** to support legacy SQL code.  The safety level used to be boolean
** and older scripts may have used numbers 0 for OFF and 1 for ON.
*/
static u8
getSafetyLevel(const char *z, int omitFull, u8 dflt)
{
	/* 123456789 123456789 123 */
	static const char zText[] = "onoffalseyestruextrafull";
	static const u8 iOffset[] = {0, 1, 2, 4, 9, 12, 15, 20};
	static const u8 iLength[] = {2, 2, 3, 5, 3, 4, 5, 4};
	static const u8 iValue[] = {1, 0, 0, 0, 1, 1, 3, 2};
	/* on no off false yes true extra full */
	int i, n;
	if (sqlite3Isdigit(*z)) {
		return (u8) sqlite3Atoi(z);
	}
	n = sqlite3Strlen30(z);
	for (i = 0; i < ArraySize(iLength); i++) {
		if (iLength[i] == n && sqlite3StrNICmp(&zText[iOffset[i]], z, n) == 0
		    && (!omitFull || iValue[i] <= 1)
		    ) {
			return iValue[i];
		}
	}
	return dflt;
}
/*
** Interpret the given string as a boolean value.
*/
u8
sqlite3GetBoolean(const char *z, u8 dflt)
{
	return getSafetyLevel(z, 1, dflt) != 0;
}
/* The sqlite3GetBoolean() function is used by other modules but the
** remainder of this file is specific to PRAGMA processing.  So omit
** the rest of the file if PRAGMAs are omitted from the build.
*/

#if !defined(SQLITE_OMIT_PRAGMA)

/*
** Set result column names for a pragma.
*/
static void
setPragmaResultColumnNames(
			   Vdbe * v,	/* The query under construction */
			   const PragmaName * pPragma	/* The pragma */
)
{
	u8 n = pPragma->nPragCName;
	sqlite3VdbeSetNumCols(v, n == 0 ? 1 : n);
	if (n == 0) {
		sqlite3VdbeSetColName(v, 0, COLNAME_NAME, pPragma->zName, SQLITE_STATIC);
	} else {
		int i, j;
		for (i = 0, j = pPragma->iPragCName; i < n; i++, j++) {
			sqlite3VdbeSetColName(v, i, COLNAME_NAME, pragCName[j], SQLITE_STATIC);
		}
	}
}
/*
** Generate code to return a single integer value.
*/
static void
returnSingleInt(Vdbe * v, i64 value)
{
	sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 1, 0, (const u8 *)&value, P4_INT64);
	sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
}
/*
** Generate code to return a single text value.
*/
static void
returnSingleText(
		 Vdbe * v,	/* Prepared statement under construction */
		 const char *zValue	/* Value to be returned */
)
{
	if (zValue) {
		sqlite3VdbeLoadString(v, 1, (const char *)zValue);
		sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
	}
}
/*
** Return a human-readable name for a constraint resolution action.
*/
#ifndef SQLITE_OMIT_FOREIGN_KEY
static const char *
actionName(u8 action)
{
	const char *zName;
	switch (action) {
	case OE_SetNull:
		zName = "SET NULL";
		break;
	case OE_SetDflt:
		zName = "SET DEFAULT";
		break;
	case OE_Cascade:
		zName = "CASCADE";
		break;
	case OE_Restrict:
		zName = "RESTRICT";
		break;
	default:
		zName = "NO ACTION";
		assert(action == OE_None);
		break;
	}
	return zName;
}
#endif


/*
** Parameter eMode must be one of the PAGER_JOURNALMODE_XXX constants
** defined in pager.h. This function returns the associated lowercase
** journal-mode name.
*/
const char *
sqlite3JournalModename(int eMode)
{
	static char *const azModeName[] = {
		"delete", "persist", "off", "truncate", "memory"
	};
	assert(PAGER_JOURNALMODE_DELETE == 0);
	assert(PAGER_JOURNALMODE_PERSIST == 1);
	assert(PAGER_JOURNALMODE_OFF == 2);
	assert(PAGER_JOURNALMODE_TRUNCATE == 3);
	assert(PAGER_JOURNALMODE_MEMORY == 4);
	assert(PAGER_JOURNALMODE_WAL == 5);
	assert(eMode >= 0 && eMode <= ArraySize(azModeName));

	if (eMode == ArraySize(azModeName))
		return 0;
	return azModeName[eMode];
}
/*
** Locate a pragma in the aPragmaName[] array.
*/
static const PragmaName *
pragmaLocate(const char *zName)
{
	int upr, lwr, mid, rc;
	lwr = 0;
	upr = ArraySize(aPragmaName) - 1;
	while (lwr <= upr) {
		mid = (lwr + upr) / 2;
		rc = sqlite3_stricmp(zName, aPragmaName[mid].zName);
		if (rc == 0)
			break;
		if (rc < 0) {
			upr = mid - 1;
		} else {
			lwr = mid + 1;
		}
	}
	return lwr > upr ? 0 : &aPragmaName[mid];
}
/*
** Process a pragma statement.
**
** Pragmas are of this form:
**
**      PRAGMA [schema.]id [= value]
**
** The identifier might also be a string.  The value is a string, and
** identifier, or a number.  If minusFlag is true, then the value is
** a number that was preceded by a minus sign.
**
** If the left side is "database.id" then pId1 is the database name
** and pId2 is the id.  If the left side is just "id" then pId1 is the
** id and pId2 is any empty string.
*/
void
sqlite3Pragma(
	      Parse * pParse,
	      Token * pId1,	/* First part of [schema.]id field */
	      Token * pId2,	/* Second part of [schema.]id field, or NULL */
	      Token * pValue,	/* Token for <value>, or NULL */
	      Token * pValue2,	/* Token for <value2>, or NULL */
	      int minusFlag	/* True if a '-' sign preceded <value> */
)
{
	char *zLeft = 0;	/* Nul-terminated UTF-8 string <id> */
	char *zRight = 0;	/* Nul-terminated UTF-8 string <value>, or NULL */
	char *zTable = 0;	/* Nul-terminated UTF-8 string <value2> or NULL */
	const char *zDb = 0;	/* The database name */
	Token *pId;		/* Pointer to <id> token */
	char *aFcntl[4];	/* Argument to SQLITE_FCNTL_PRAGMA */
	int iDb;		/* Database index for <database> */
	int rc;			/* return value form SQLITE_FCNTL_PRAGMA */
	sqlite3 *db = pParse->db;	/* The database connection */
	Db *pDb;		/* The specific database being pragmaed */
	Vdbe *v = sqlite3GetVdbe(pParse);	/* Prepared statement */
	const PragmaName *pPragma;	/* The pragma */
	struct session *user_session = current_session();

	if (v == 0)
		return;
	sqlite3VdbeRunOnlyOnce(v);
	pParse->nMem = 2;

	/* Interpret the [schema.] part of the pragma statement. iDb is the *
	 * index of the database this pragma is being applied to in db.aDb[]. */
	iDb = sqlite3TwoPartName(pParse, pId1, &pId);
	if (iDb < 0)
		return;
	pDb = &db->mdb;

	zLeft = sqlite3NameFromToken(db, pId);
	if (!zLeft)
		return;
	if (minusFlag) {
		zRight = sqlite3MPrintf(db, "-%T", pValue);
	} else {
		zRight = sqlite3NameFromToken(db, pValue);
	}
	zTable = sqlite3NameFromToken(db, pValue2);

	zDb = 0;
	if (sqlite3AuthCheck(pParse, SQLITE_PRAGMA, zLeft, zRight, zDb)) {
		goto pragma_out;
	}
	/* Send an SQLITE_FCNTL_PRAGMA file-control to the underlying VFS *
	 * connection.  If it returns SQLITE_OK, then assume that the VFS *
	 * handled the pragma and generate a no-op prepared statement. *
	 * 
	 * IMPLEMENTATION-OF: R-12238-55120 Whenever a PRAGMA
	 * statement is parsed, an SQLITE_FCNTL_PRAGMA file control
	 * is sent to the open sqlite3_file
	 * object corresponding to the database file to which the pragma *
	 * statement refers. *
	 *
	 * IMPLEMENTATION-OF: R-29875-31678 The argument to the
	 * SQLITE_FCNTL_PRAGMA * file control is an array of pointers to
	 * strings (char**) in which the * second element of the array is the
	 * name of the pragma and the third * element is the argument to the
	 * pragma or NULL if the pragma has no * argument. */
	aFcntl[0] = 0;
	aFcntl[1] = zLeft;
	aFcntl[2] = zRight;
	aFcntl[3] = 0;
	db->busyHandler.nBusy = 0;
	rc = sqlite3_file_control(db, zDb,
				  SQLITE_FCNTL_PRAGMA, (void *)aFcntl);
	if (rc == SQLITE_OK) {
		sqlite3VdbeSetNumCols(v, 1);
		sqlite3VdbeSetColName(v, 0, COLNAME_NAME,
				      aFcntl[0], SQLITE_TRANSIENT);
		returnSingleText(v, aFcntl[0]);
		sqlite3_free(aFcntl[0]);
		goto pragma_out;
	}
	if (rc != SQLITE_NOTFOUND) {
		if (aFcntl[0]) {
			sqlite3ErrorMsg(pParse, "%s", aFcntl[0]);
			sqlite3_free(aFcntl[0]);
		}
		pParse->nErr++;
		pParse->rc = rc;
		goto pragma_out;
	}
	/* Locate the pragma in the lookup table */
	pPragma = pragmaLocate(zLeft);
	if (pPragma == 0)
		goto pragma_out;

	/* Make sure the database schema is loaded if the pragma requires that */
	if ((pPragma->mPragFlg & PragFlg_NeedSchema) != 0) {
		if (sqlite3ReadSchema(pParse))
			goto pragma_out;
	}
	/* Register the result column names for pragmas that return results */
	if ((pPragma->mPragFlg & PragFlg_NoColumns) == 0
	    && ((pPragma->mPragFlg & PragFlg_NoColumns1) == 0 || zRight == 0)
	    ) {
		setPragmaResultColumnNames(v, pPragma);
	}
	/* Jump to the appropriate pragma handler */
	switch (pPragma->ePragTyp) {

#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
	/* *  PRAGMA [schema.]secure_delete *  PRAGMA
	 * [schema.]secure_delete=ON/OFF *
	 *
	 * The first form reports the current setting for the *
	 * secure_delete flag.  The second form changes the
	 * secure_delete * flag setting and reports thenew value. */
	case PragTyp_SECURE_DELETE:{
		Btree *pBt = pDb->pBt;
		int b = -1;
		assert(pBt != 0);
		if (zRight) {
			b = sqlite3GetBoolean(zRight, 0);
		}
		if (pId2->n == 0 && b >= 0) {
			sqlite3BtreeSecureDelete(db->mdb.pBt, b);
		}
		b = sqlite3BtreeSecureDelete(pBt, b);
		returnSingleInt(v, b);
		break;
	}
#endif		/* SQLITE_OMIT_PAGER_PRAGMAS */

#ifndef SQLITE_OMIT_PAGER_PRAGMAS
	/* *   PRAGMA [schema.]synchronous *   PRAGMA
	 * [schema.]synchronous=OFF|ON|NORMAL|FULL|EXTRA *
	 *
	 * Return or set the local value of the synchronous flag. Changing *
	 * the local value does not make changes to the disk file and
	 * the * default value will be restored the next time the
	 * database is * opened. */
	case PragTyp_SYNCHRONOUS:{
		if (!zRight) {
			returnSingleInt(v, pDb->safety_level - 1);
		} else {
			/* * Autocommit is default VDBE state. Only
			 * OP_Savepoint may change it to 0 * Thats why
			 * we shouldn't check it */
			int iLevel = (getSafetyLevel(zRight, 0, 1) + 1) & PAGER_SYNCHRONOUS_MASK;
			if (iLevel == 0)
				iLevel = 1;
			pDb->safety_level = iLevel;
			pDb->bSyncSet = 1;
		}
		break;
	}
#endif		/* SQLITE_OMIT_PAGER_PRAGMAS */

#ifndef SQLITE_OMIT_FLAG_PRAGMAS
	case PragTyp_FLAG:{
		if (zRight == 0) {
			setPragmaResultColumnNames(v, pPragma);
			returnSingleInt(v, (user_session->sql_flags & pPragma->iArg) != 0);
		} else {
			int mask = pPragma->iArg;	/* Mask of bits to set
							 * or clear. */

			if (sqlite3GetBoolean(zRight, 0)) {
				user_session->sql_flags |= mask;
			} else {
				user_session->sql_flags &= ~mask;
				if (mask == SQLITE_DeferFKs) {
					Vdbe *v = db->pVdbe;
					while (v->pNext) {
						v->nDeferredImmCons = 0;
						v = v->pNext;
					}
				}
			}

			/* Many of the flag-pragmas modify the code
			 * generated by the SQL * compiler (eg.
			 * count_changes). So add an opcode to expire
			 * all * compiled SQL statements after
			 * modifying a pragma value. */
		sqlite3VdbeAddOp0(v, OP_Expire);
		}
		break;
	}
#endif		/* SQLITE_OMIT_FLAG_PRAGMAS */

#ifndef SQLITE_OMIT_SCHEMA_PRAGMAS
	/* *   PRAGMA table_info(<table>) *
	 *
	 * Return a single row for each column of the named table. The
	 * columns of * the returned data set are: *
	 *
	 * cid:        Column id (numbered from left to right, starting at
	 * 0) * name:       Column name * type:       Column
	 * declaration type. * notnull:    True if 'NOT NULL' is part
	 * of column declaration * dflt_value: The default value for
	 * the column, if any. */
	case PragTyp_TABLE_INFO:
		if (zRight) {
			Table *pTab;
			pTab = sqlite3LocateTable(pParse, LOCATE_NOERR, zRight);
			if (pTab) {
				int i, k;
				int nHidden = 0;
				Column *pCol;
				Index *pPk = sqlite3PrimaryKeyIndex(pTab);
				pParse->nMem = 6;
				sqlite3CodeVerifySchema(pParse);
				sqlite3ViewGetColumnNames(pParse, pTab);
				for (i = 0, pCol = pTab->aCol; i < pTab->nCol; i++, pCol++) {
					if (IsHiddenColumn(pCol)) {
						nHidden++;
						continue;
					}
					if ((pCol->colFlags & COLFLAG_PRIMKEY) == 0) {
						k = 0;
					} else if (pPk == 0) {
						k = 1;
					} else {
						for (k = 1; k <= pTab->nCol && pPk->aiColumn[k - 1] != i; k++) {
						}
					}
					assert(pCol->pDflt == 0 || pCol->pDflt->op == TK_SPAN);
					sqlite3VdbeMultiLoad(v, 1, "issisi",
   						  	    i - nHidden,
							    pCol->zName,
					  		    sqlite3ColumnType(pCol, ""),
						            pCol->notNull ? 1 : 0,
							    pCol->pDflt ? pCol->pDflt->u.zToken : 0,
							    k);
					sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 6);
				}
			}
		}
		break;

	case PragTyp_STATS:{
		Index *pIdx;
		HashElem *i;
		pParse->nMem = 4;
		sqlite3CodeVerifySchema(pParse);
		for (i = sqliteHashFirst(&pDb->pSchema->tblHash); i; i = sqliteHashNext(i)) {
			Table *pTab = sqliteHashData(i);
			sqlite3VdbeMultiLoad(v, 1, "ssii",
					     pTab->zName,
					     0,
					     pTab->szTabRow,
					     pTab->nRowLogEst);
			sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 4);
			for (pIdx = pTab->pIndex; pIdx; pIdx = pIdx->pNext) {
				sqlite3VdbeMultiLoad(v, 2, "sii",
						     pIdx->zName,
						     pIdx->szIdxRow,
						pIdx->aiRowLogEst[0]);
				sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 4);
			}
		}
		break;
	}


	case PragTyp_INDEX_INFO:{
		if (zRight && zTable) {
			Index *pIdx;
			pIdx = sqlite3LocateIndex(db, zRight, zTable);
			if (pIdx) {
				int i;
				int mx;
				if (pPragma->iArg) {
					/* PRAGMA index_xinfo (newer
					 * version with more rows and
					 * columns) */
					mx = pIdx->nColumn;
					pParse->nMem = 6;
				} else {
					/* PRAGMA index_info (legacy
					 * version) */
					mx = pIdx->nKeyCol;
					pParse->nMem = 3;
				}
				sqlite3CodeVerifySchema(pParse);
				assert(pParse->nMem <= pPragma->nPragCName);
				for (i = 0; i < mx; i++) {
					i16 cnum = pIdx->aiColumn[i];
					assert(pIdx->pTable);
					sqlite3VdbeMultiLoad(v, 1, "iis", i, cnum,
							     cnum < 0 ? 0 : pIdx->pTable->aCol[cnum].zName);
					if (pPragma->iArg) {
						sqlite3VdbeMultiLoad(v, 4, "isi",
						  		    pIdx->aSortOrder[i],
								    pIdx->azColl[i],
						   		    i < pIdx->nKeyCol);
					}
					sqlite3VdbeAddOp2(v, OP_ResultRow, 1, pParse->nMem);
				}
			}
		}
		break;
	}
	case PragTyp_INDEX_LIST:{
		if (zRight) {
			Index *pIdx;
			Table *pTab;
			int i;
			pTab = sqlite3FindTable(db, zRight);
			if (pTab) {
				pParse->nMem = 5;
				sqlite3CodeVerifySchema(pParse);
				for (pIdx = pTab->pIndex, i = 0; pIdx; pIdx = pIdx->pNext, i++) {
					const char *azOrigin[] = {"c", "u", "pk"};
					sqlite3VdbeMultiLoad(v, 1, "isisi",
							     i,
							     pIdx->zName,
						             IsUniqueIndex(pIdx),
					      		     azOrigin[pIdx->idxType],
						    	     pIdx->pPartIdxWhere != 0);
							     sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 5);
					}
			}
		}
		break;
	}

	case PragTyp_DATABASE_LIST:{
		pParse->nMem = 3;
		assert(db->mdb.pBt == 0);
		assert(db->mdb.zDbSName != 0);
		sqlite3VdbeMultiLoad(v, 1, "iss",
				     0,
				     db->mdb.zDbSName,
				     sqlite3BtreeGetFilename(db->mdb.pBt));
		sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 3);
		break;
	}

	case PragTyp_COLLATION_LIST: {
		int i = 0;
		HashElem *p;
		pParse->nMem = 2;
		for (p = sqliteHashFirst(&db->aCollSeq); p; p = sqliteHashNext(p)) {
			CollSeq *pColl = (CollSeq *) sqliteHashData(p);
			sqlite3VdbeMultiLoad(v, 1, "is", i++, pColl->zName);
			sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
		}
		break;
	}
#endif		/* SQLITE_OMIT_SCHEMA_PRAGMAS */

#ifndef SQLITE_OMIT_FOREIGN_KEY
	case PragTyp_FOREIGN_KEY_LIST:{
		if (zRight) {
			FKey *pFK;
			Table *pTab;
			pTab = sqlite3FindTable(db, zRight);
			if (pTab) {
				pFK = pTab->pFKey;
				if (pFK) {
					int i = 0;
					pParse->nMem = 8;
					sqlite3CodeVerifySchema(pParse);
					while (pFK) {
						int j;
						for (j = 0; j < pFK->nCol; j++) {
							sqlite3VdbeMultiLoad(v, 1, "iissssss",
									     i,
									     j,
									     pFK->zTo,
									     pTab->aCol[pFK->aCol[j].iFrom].zName,
									     pFK->aCol[j].zCol,
								             actionName(pFK->aAction[1]),	/* ON UPDATE */
									     actionName(pFK->aAction[0]),	/* ON DELETE */
								      	     "NONE");
							sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 8);
						}
						++i;
						pFK = pFK->pNextFrom;
					}
				}
			}
		}
		break;
	}
#endif		/* !defined(SQLITE_OMIT_FOREIGN_KEY) */

#ifndef SQLITE_OMIT_FOREIGN_KEY
#ifndef SQLITE_OMIT_TRIGGER
	case PragTyp_FOREIGN_KEY_CHECK:{
		FKey *pFK;	/* A foreign key constraint */
		Table *pTab;	/* Child table contain "REFERENCES"
				 * keyword */
		Table *pParent;	/* Parent table that child points to */
		Index *pIdx;	/* Index in the parent table */
		int i;	/* Loop counter:  Foreign key number for pTab */
		int j;	/* Loop counter:  Field of the foreign key */
		HashElem *k;	/* Loop counter:  Next table in schema */
		int x;	/* result variable */
		int regResult;	/* 3 registers to hold a result row */
		int regKey;	/* Register to hold key for checking
				 * the FK */
		int regRow;	/* Registers to hold a row from pTab */
		int addrTop;	/* Top of a loop checking foreign keys */
		int addrOk;	/* Jump here if the key is OK */
		int *aiCols;	/* child to parent column mapping */

		regResult = pParse->nMem + 1;
		pParse->nMem += 4;
		regKey = ++pParse->nMem;
		regRow = ++pParse->nMem;
		assert(iDb == 0);
		sqlite3CodeVerifySchema(pParse);
		k = sqliteHashFirst(&db->mdb.pSchema->tblHash);
		while (k) {
			if (zRight) {
				pTab = sqlite3LocateTable(pParse, 0, zRight);
				k = 0;
			} else {
				pTab = (Table *) sqliteHashData(k);
				k = sqliteHashNext(k);
			}
			if (pTab == 0 || pTab->pFKey == 0)
				continue;
			sqlite3TableLock(pParse, pTab->tnum, 0, pTab->zName);
			if (pTab->nCol + regRow > pParse->nMem)
				pParse->nMem = pTab->nCol + regRow;
			sqlite3OpenTable(pParse, 0, pTab, OP_OpenRead);
			sqlite3VdbeLoadString(v, regResult, pTab->zName);
			for (i = 1, pFK = pTab->pFKey; pFK; i++, pFK = pFK->pNextFrom) {
				pParent = sqlite3FindTable(db, pFK->zTo);
				if (pParent == 0)
					continue;
				pIdx = 0;
				sqlite3TableLock(pParse, pParent->tnum, 0, pParent->zName);
				x = sqlite3FkLocateIndex(pParse, pParent, pFK, &pIdx, 0);
				if (x == 0) {
					if (pIdx == 0) {
						sqlite3OpenTable(pParse, i, pParent, OP_OpenRead);
					} else {
						sqlite3VdbeAddOp3(v, OP_OpenRead, i, pIdx->tnum, iDb);
						sqlite3VdbeSetP4KeyInfo(pParse, pIdx);
					}
				} else {
					k = 0;
					break;
				}
			}
			assert(pParse->nErr > 0 || pFK == 0);
			if (pFK)
				break;
			if (pParse->nTab < i)
				pParse->nTab = i;
			addrTop = sqlite3VdbeAddOp1(v, OP_Rewind, 0);
			VdbeCoverage(v);
			for (i = 1, pFK = pTab->pFKey; pFK; i++, pFK = pFK->pNextFrom) {
				pParent = sqlite3FindTable(db, pFK->zTo);
				pIdx = 0;
				aiCols = 0;
				if (pParent) {
					x = sqlite3FkLocateIndex(pParse, pParent, pFK, &pIdx, &aiCols);
					assert(x == 0);
				}
				addrOk = sqlite3VdbeMakeLabel(v);
				if (pParent && pIdx == 0) {
					int iKey = pFK->aCol[0].iFrom;
					assert(iKey >= 0 && iKey < pTab->nCol);
					if (iKey != pTab->iPKey) {
						sqlite3VdbeAddOp3(v, OP_Column, 0, iKey, regRow);
						sqlite3ColumnDefault(v, pTab, iKey, regRow);
						sqlite3VdbeAddOp2(v, OP_IsNull, regRow, addrOk);
						VdbeCoverage(v);
					} else {
						sqlite3VdbeAddOp2(v, OP_Rowid, 0, regRow);
					}
					sqlite3VdbeAddOp3(v, OP_SeekRowid, i, 0, regRow);
					VdbeCoverage(v);
					sqlite3VdbeGoto(v, addrOk);
					sqlite3VdbeJumpHere(v, sqlite3VdbeCurrentAddr(v) - 2);
				} else {
					for (j = 0; j < pFK->nCol; j++) {
						sqlite3ExprCodeGetColumnOfTable(v, pTab, 0,
										aiCols ? aiCols[j] : pFK->aCol[j].iFrom, regRow + j);
						sqlite3VdbeAddOp2(v, OP_IsNull, regRow + j, addrOk);
						VdbeCoverage(v);
					}
					if (pParent) {
						sqlite3VdbeAddOp4(v, OP_MakeRecord, regRow, pFK->nCol, regKey,
								  sqlite3IndexAffinityStr(db, pIdx), pFK->nCol);
						sqlite3VdbeAddOp4Int(v, OP_Found, i, addrOk, regKey, 0);
						VdbeCoverage(v);
					}
				}
				sqlite3VdbeAddOp2(v, OP_Rowid, 0, regResult + 1);
				sqlite3VdbeMultiLoad(v, regResult + 2, "si", pFK->zTo, i - 1);
				sqlite3VdbeAddOp2(v, OP_ResultRow, regResult, 4);
				sqlite3VdbeResolveLabel(v, addrOk);
				sqlite3DbFree(db, aiCols);
			}
			sqlite3VdbeAddOp2(v, OP_Next, 0, addrTop + 1);
			VdbeCoverage(v);
			sqlite3VdbeJumpHere(v, addrTop);
		}
		break;
	}
#endif		/* !defined(SQLITE_OMIT_TRIGGER) */
#endif				/* !defined(SQLITE_OMIT_FOREIGN_KEY) */

#ifndef NDEBUG
	case PragTyp_PARSER_TRACE:{
		if (zRight) {
			if (sqlite3GetBoolean(zRight, 0)) {
				sqlite3ParserTrace(stdout, "parser: ");
			} else {
				sqlite3ParserTrace(0, 0);
			}
		}
		break;
	}
#endif

		/* Reinstall the LIKE and GLOB functions.  The variant of LIKE *
		 * used will be case sensitive or not depending on the RHS. */
	case PragTyp_CASE_SENSITIVE_LIKE:{
		if (zRight) {
			sqlite3RegisterLikeFunctions(db, sqlite3GetBoolean(zRight, 0));
		}
		break;
	}

#ifndef SQLITE_INTEGRITY_CHECK_ERROR_MAX
#define SQLITE_INTEGRITY_CHECK_ERROR_MAX 100
#endif

	/* *   PRAGMA encoding *   PRAGMA encoding = "utf-8" *
	 *
	 * In its first form, this pragma returns the encoding of
	 * the main * database. If the database is not
	 * initialized, it is initialized now. *
	 *
	 * The second form of this pragma is a no-op if the main
	 * database file * has not already been initialized. In
	 * this case it sets the default * encoding that will
	 * be used for the main database file if a new file *
	 * is created. If an existing main database file is
	 * opened, then the * default text encoding for the
	 * existing database is used. *
	 *
	 * In all cases new databases created using the ATTACH
	 * command are * created to use the same default text
	 * encoding as the main database. If * the main
	 * database has not been initialized and/or created
	 * when ATTACH * is executed, this is done before the
	 * ATTACH operation. *
	 *
	 * In the second form this pragma sets the text encoding
	 * to be used in * new database files created using
	 * this database handle. It is only * useful if invoked
	 * immediately after the main database i */
	case PragTyp_ENCODING:{
		static const struct EncName {
			char *zName;
			u8 enc;
		} encnames[] = {
			{
				"UTF8", SQLITE_UTF8
			},
			{
				"UTF-8", SQLITE_UTF8
			},	/* Must be element [1] */
			{
				0, 0
			}
		};
		const struct EncName *pEnc;
		if (!zRight) {	/* "PRAGMA encoding" */
			if (sqlite3ReadSchema(pParse))
				goto pragma_out;
			assert(encnames[SQLITE_UTF8].enc == SQLITE_UTF8);
			returnSingleText(v, encnames[ENC(pParse->db)].zName);
		} else {
			/* "PRAGMA encoding = XXX" */
			/* Only change the value of sqlite.enc
			 * if the database handle is not *
			 * initialized. If the main database
			 * exists, the new sqlite.enc value *
			 * will be overwritten when the schema
			 * is next loaded. If it does not *
			 * already exists, it will be created
			 * to use the new encoding value. */
			if (
			    !(DbHasProperty(db, DB_SchemaLoaded)) ||
			    DbHasProperty(db, DB_Empty)
			    ) {
				for (pEnc = &encnames[0]; pEnc->zName; pEnc++) {
					if (0 == sqlite3StrICmp(zRight, pEnc->zName)) {
						SCHEMA_ENC(db) = ENC(db) = pEnc->enc;
						break;
					}
				}
				if (!pEnc->zName) {
					sqlite3ErrorMsg(pParse, "unsupported encoding: %s", zRight);
				}
			}
		}
		break;
	}

#ifndef SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS
	/* *   PRAGMA [schema.]schema_version *   PRAGMA
	 * [schema.]schema_version = <integer> *
	 *
	 * PRAGMA [schema.]user_version *   PRAGMA
	 * [schema.]user_version = <integer> *
	 *
	 * PRAGMA [schema.]freelist_count *
	 *
	 * PRAGMA [schema.]data_version *
	 *
	 * PRAGMA [schema.]application_id *   PRAGMA
	 * [schema.]application_id = <integer> *
	 *
	 * The pragma's schema_version and user_version are used
	 * to set or get * the value of the schema-version and
	 * user-version, respectively. Both * the
	 * schema-version and the user-version are 32-bit
	 * signed integers * stored in the database header. *
	 *
	 * The schema-cookie is usually only manipulated
	 * internally by SQLite. It * is incremented by SQLite
	 * whenever the database schema is modified (by *
	 * creating or dropping a table or index). The schema
	 * version is used by * SQLite each time a query is
	 * executed to ensure that the internal cache * of the
	 * schema used when compiling the SQL query matches the
	 * schema of * the database against which the compiled
	 * query is actually executed. * Subverting this
	 * mechanism by using "PRAGMA schema_version" to modify *
	 * the schema-version is potentially dangerous and may
	 * lead to program * crashes or database corruption.
	 * Use with caution! *
	 *
	 * The user-version is not used internally by SQLite. It
	 * may be used by * applications for any purpose. */
	case PragTyp_HEADER_VALUE:{
		int iCookie = pPragma->iArg;	/* Which cookie to read
						 * or write */
		sqlite3VdbeUsesBtree(v);
		if (zRight && (pPragma->mPragFlg & PragFlg_ReadOnly) == 0) {
			/* Write the specified cookie value */
			static const VdbeOpList setCookie[] = {
				{OP_Transaction, 0, 1, 0},	/* 0 */
				{OP_SetCookie, 0, 0, 0},	/* 1 */
			};
			VdbeOp *aOp;
			sqlite3VdbeVerifyNoMallocRequired(v, ArraySize(setCookie));
			aOp = sqlite3VdbeAddOpList(v, ArraySize(setCookie), setCookie, 0);
			if (ONLY_IF_REALLOC_STRESS(aOp == 0))
				break;
			aOp[0].p1 = iDb;
			aOp[1].p1 = iDb;
			aOp[1].p2 = iCookie;
			aOp[1].p3 = sqlite3Atoi(zRight);
		} else {
			/* Read the specified cookie value */
			static const VdbeOpList readCookie[] = {
				{OP_Transaction, 0, 0, 0},	/* 0 */
				{OP_ReadCookie, 0, 1, 0},	/* 1 */
				{OP_ResultRow, 1, 1, 0}
			};
			VdbeOp *aOp;
			sqlite3VdbeVerifyNoMallocRequired(v, ArraySize(readCookie));
			aOp = sqlite3VdbeAddOpList(v, ArraySize(readCookie), readCookie, 0);
			if (ONLY_IF_REALLOC_STRESS(aOp == 0))
				break;
			aOp[0].p1 = iDb;
			aOp[1].p1 = iDb;
			aOp[1].p3 = iCookie;
			sqlite3VdbeReusable(v);
		}
		break;
	}

/* Tarantool: TODO: comment this so far, since native SQLite WAL was remoced.
   This might be used with native Tarantool's WAL.  */
#if 0
			/* *   PRAGMA [schema.]wal_checkpoint =
			 * passive|full|restart|truncate *
			 *
			 * Checkpoint the database. */
	case PragTyp_WAL_CHECKPOINT:{
				int iBt = (pId2->z ? iDb : SQLITE_MAX_ATTACHED);
				int eMode = SQLITE_CHECKPOINT_PASSIVE;
				if (zRight) {
					if (sqlite3StrICmp(zRight, "full") == 0) {
						eMode = SQLITE_CHECKPOINT_FULL;
					} else if (sqlite3StrICmp(zRight, "restart") == 0) {
						eMode = SQLITE_CHECKPOINT_RESTART;
					} else if (sqlite3StrICmp(zRight, "truncate") == 0) {
						eMode = SQLITE_CHECKPOINT_TRUNCATE;
					}
				}
				pParse->nMem = 3;
				sqlite3VdbeAddOp3(v, OP_Checkpoint, iBt, eMode, 1);
				sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 3);
			}
			break;

			/* *   PRAGMA wal_autocheckpoint *   PRAGMA
			 * wal_autocheckpoint = N *
			 *
			 * Configure a database connection to automatically
			 * checkpoint a database * after accumulating N frames
			 * in the log. Or query for the current value * of N. */
	case PragTyp_WAL_AUTOCHECKPOINT:{
				if (zRight) {
					sqlite3_wal_autocheckpoint(db, sqlite3Atoi(zRight));
				}
				returnSingleInt(v,
				   db->xWalCallback == sqlite3WalDefaultHook ?
					  SQLITE_PTR_TO_INT(db->pWalArg) : 0);
			}
			break;
#endif

	/* *  PRAGMA shrink_memory *
	 *
	 * IMPLEMENTATION-OF: R-23445-46109 This pragma causes the
	 * database * connection on which it is invoked to free
	 * up as much memory as it * can, by calling
	 * sqlite3_db_release_memory(). */
	case PragTyp_SHRINK_MEMORY:
		sqlite3_db_release_memory(db);
		break;

	/* *   PRAGMA busy_timeout *   PRAGMA busy_timeout = N *
	 *
	 * Call sqlite3_busy_timeout(db, N).  Return the current
	 * timeout value * if one is set.  If no busy handler
	 * or a different busy handler is set * then 0 is
	 * returned.  Setting the busy_timeout to 0 or negative *
	 * disables the timeout. */
	/* case PragTyp_BUSY_TIMEOUT */
	default: {
		assert(pPragma->ePragTyp == PragTyp_BUSY_TIMEOUT);
		if (zRight) {
			sqlite3_busy_timeout(db, sqlite3Atoi(zRight));
		}
		returnSingleInt(v, db->busyTimeout);
		break;
	}

	/* *   PRAGMA soft_heap_limit *   PRAGMA
	 * soft_heap_limit = N *
	 *
	 * IMPLEMENTATION-OF: R-26343-45930 This pragma invokes
	 * the * sqlite3_soft_heap_limit64() interface with the
	 * argument N, if N is * specified and is a
	 * non-negative integer. * IMPLEMENTATION-OF:
	 * R-64451-07163 The soft_heap_limit pragma always *
	 * returns the same integer that would be returned by
	 * the * sqlite3_soft_heap_limit64(-1) C-language
	 * function. */
	case PragTyp_SOFT_HEAP_LIMIT:{
		sqlite3_int64 N;
		if (zRight && sqlite3DecOrHexToI64(zRight, &N) == SQLITE_OK) {
			sqlite3_soft_heap_limit64(N);
		}
		returnSingleInt(v, sqlite3_soft_heap_limit64(-1));
		break;
	}

	/* *   PRAGMA threads *   PRAGMA threads = N *
	 *
	 * Configure the maximum number of worker threads. Return
	 * the new * maximum, which might be less than
	 * requested. */
	case PragTyp_THREADS:{
		sqlite3_int64 N;
		if (zRight
		    && sqlite3DecOrHexToI64(zRight, &N) == SQLITE_OK
		    && N >= 0
		    ) {
			sqlite3_limit(db, SQLITE_LIMIT_WORKER_THREADS, (int)(N & 0x7fffffff));
		}
		returnSingleInt(v, sqlite3_limit(db, SQLITE_LIMIT_WORKER_THREADS, -1));
		break;
	}

#ifdef SQLITE_HAS_CODEC
	case PragTyp_KEY:{
		if (zRight)
			sqlite3_key_v2(db, zDb, zRight, sqlite3Strlen30(zRight));
		break;
	}
	case PragTyp_REKEY:{
		if (zRight)
			sqlite3_rekey_v2(db, zDb, zRight, sqlite3Strlen30(zRight));
		break;
	}
	case PragTyp_HEXKEY:{
		if (zRight) {
			u8 iByte;
			int i;
			char zKey[40];
			for (i = 0, iByte = 0; i < sizeof(zKey) * 2 && sqlite3Isxdigit(zRight[i]); i++) {
				iByte = (iByte << 4) + sqlite3HexToInt(zRight[i]);
				if ((i & 1) != 0)
					zKey[i / 2] = iByte;
			}
			if ((zLeft[3] & 0xf) == 0xb) {
				sqlite3_key_v2(db, zDb, zKey, i / 2);
			} else {
				sqlite3_rekey_v2(db, zDb, zKey, i / 2);
			}
		}
		break;
	}
#endif
#if defined(SQLITE_HAS_CODEC) || defined(SQLITE_ENABLE_CEROD)
	case PragTyp_ACTIVATE_EXTENSIONS:
		if (zRight) {
#ifdef SQLITE_HAS_CODEC
			if (sqlite3StrNICmp(zRight, "see-", 4) == 0) {
				sqlite3_activate_see(&zRight[4]);
			}
#endif
#ifdef SQLITE_ENABLE_CEROD
			if (sqlite3StrNICmp(zRight, "cerod-", 6) == 0) {
				sqlite3_activate_cerod(&zRight[6]);
			}
#endif
		}
		break;
#endif

	}		/* End of the PRAGMA switch */

	/* The following block is a no-op unless SQLITE_DEBUG is
	 * defined. Its only * purpose is to execute assert()
	 * statements to verify that if the * PragFlg_NoColumns1 flag
	 * is set and the caller specified an argument * to the PRAGMA,
	 * the implementation has not added any OP_ResultRow *
	 * instructions to the VM.  */
	if ((pPragma->mPragFlg & PragFlg_NoColumns1) && zRight) {
		sqlite3VdbeVerifyNoResultRow(v);
	}
pragma_out:
	sqlite3DbFree(db, zLeft);
	sqlite3DbFree(db, zRight);
	sqlite3DbFree(db, zTable);
}
#ifndef SQLITE_OMIT_VIRTUALTABLE
/*****************************************************************************
** Implementation of an eponymous virtual table that runs a pragma.
**
*/
typedef struct PragmaVtab PragmaVtab;
typedef struct PragmaVtabCursor PragmaVtabCursor;
struct PragmaVtab {
	sqlite3_vtab base;	/* Base class.  Must be first */
	sqlite3 *db;		/* The database connection to which it belongs */
	const PragmaName *pName;/* Name of the pragma */
	u8 nHidden;		/* Number of hidden columns */
	u8 iHidden;		/* Index of the first hidden column */
};
struct PragmaVtabCursor {
	sqlite3_vtab_cursor base;	/* Base class.  Must be first */
	sqlite3_stmt *pPragma;	/* The pragma statement to run */
	sqlite_int64 iRowid;	/* Current rowid */
	char *azArg[2];		/* Value of the argument and schema */
};
/*
** Pragma virtual table module xConnect method.
*/
static int
pragmaVtabConnect(
		  sqlite3 * db,
		  void *pAux,
		  int argc, const char *const *argv,
		  sqlite3_vtab ** ppVtab,
		  char **pzErr
)
{
	const PragmaName *pPragma = (const PragmaName *)pAux;
	PragmaVtab *pTab = 0;
	int rc;
	int i, j;
	char cSep = '(';
	StrAccum acc;
	char zBuf[200];

	UNUSED_PARAMETER(argc);
	UNUSED_PARAMETER(argv);
	sqlite3StrAccumInit(&acc, 0, zBuf, sizeof(zBuf), 0);
	sqlite3StrAccumAppendAll(&acc, "CREATE TABLE x");
	for (i = 0, j = pPragma->iPragCName; i < pPragma->nPragCName; i++, j++) {
		sqlite3XPrintf(&acc, "%c\"%s\"", cSep, pragCName[j]);
		cSep = ',';
	}
	if (i == 0) {
		sqlite3XPrintf(&acc, "(\"%s\"", pPragma->zName);
		cSep = ',';
		i++;
	}
	j = 0;
	if (pPragma->mPragFlg & PragFlg_Result1) {
		sqlite3StrAccumAppendAll(&acc, ",arg HIDDEN");
		j++;
	}
	if (pPragma->mPragFlg & (PragFlg_SchemaOpt | PragFlg_SchemaReq)) {
		sqlite3StrAccumAppendAll(&acc, ",schema HIDDEN");
		j++;
	}
	sqlite3StrAccumAppend(&acc, ")", 1);
	sqlite3StrAccumFinish(&acc);
	assert(strlen(zBuf) < sizeof(zBuf) - 1);
	rc = sqlite3_declare_vtab(db, zBuf);
	if (rc == SQLITE_OK) {
		pTab = (PragmaVtab *) sqlite3_malloc(sizeof(PragmaVtab));
		if (pTab == 0) {
			rc = SQLITE_NOMEM;
		} else {
			memset(pTab, 0, sizeof(PragmaVtab));
			pTab->pName = pPragma;
			pTab->db = db;
			pTab->iHidden = i;
			pTab->nHidden = j;
		}
	} else {
		*pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
	}

	*ppVtab = (sqlite3_vtab *) pTab;
	return rc;
}
/*
** Pragma virtual table module xDisconnect method.
*/
static int
pragmaVtabDisconnect(sqlite3_vtab * pVtab)
{
	PragmaVtab *pTab = (PragmaVtab *) pVtab;
	sqlite3_free(pTab);
	return SQLITE_OK;
}
/* Figure out the best index to use to search a pragma virtual table.
**
** There are not really any index choices.  But we want to encourage the
** query planner to give == constraints on as many hidden parameters as
** possible, and especially on the first hidden parameter.  So return a
** high cost if hidden parameters are unconstrained.
*/
static int
pragmaVtabBestIndex(sqlite3_vtab * tab, sqlite3_index_info * pIdxInfo)
{
	PragmaVtab *pTab = (PragmaVtab *) tab;
	const struct sqlite3_index_constraint *pConstraint;
	int i, j;
	int seen[2];

	pIdxInfo->estimatedCost = (double)1;
	if (pTab->nHidden == 0) {
		return SQLITE_OK;
	}
	pConstraint = pIdxInfo->aConstraint;
	seen[0] = 0;
	seen[1] = 0;
	for (i = 0; i < pIdxInfo->nConstraint; i++, pConstraint++) {
		if (pConstraint->usable == 0)
			continue;
		if (pConstraint->op != SQLITE_INDEX_CONSTRAINT_EQ)
			continue;
		if (pConstraint->iColumn < pTab->iHidden)
			continue;
		j = pConstraint->iColumn - pTab->iHidden;
		assert(j < 2);
		seen[j] = i + 1;
	}
	if (seen[0] == 0) {
		pIdxInfo->estimatedCost = (double)2147483647;
		pIdxInfo->estimatedRows = 2147483647;
		return SQLITE_OK;
	}
	j = seen[0] - 1;
	pIdxInfo->aConstraintUsage[j].argvIndex = 1;
	pIdxInfo->aConstraintUsage[j].omit = 1;
	if (seen[1] == 0)
		return SQLITE_OK;
	pIdxInfo->estimatedCost = (double)20;
	pIdxInfo->estimatedRows = 20;
	j = seen[1] - 1;
	pIdxInfo->aConstraintUsage[j].argvIndex = 2;
	pIdxInfo->aConstraintUsage[j].omit = 1;
	return SQLITE_OK;
}
/* Create a new cursor for the pragma virtual table */
static int
pragmaVtabOpen(sqlite3_vtab * pVtab, sqlite3_vtab_cursor ** ppCursor)
{
	PragmaVtabCursor *pCsr;
	pCsr = (PragmaVtabCursor *) sqlite3_malloc(sizeof(*pCsr));
	if (pCsr == 0)
		return SQLITE_NOMEM;
	memset(pCsr, 0, sizeof(PragmaVtabCursor));
	pCsr->base.pVtab = pVtab;
	*ppCursor = &pCsr->base;
	return SQLITE_OK;
}
/* Clear all content from pragma virtual table cursor. */
static void
pragmaVtabCursorClear(PragmaVtabCursor * pCsr)
{
	int i;
	sqlite3_finalize(pCsr->pPragma);
	pCsr->pPragma = 0;
	for (i = 0; i < ArraySize(pCsr->azArg); i++) {
		sqlite3_free(pCsr->azArg[i]);
		pCsr->azArg[i] = 0;
	}
}
/* Close a pragma virtual table cursor */
static int
pragmaVtabClose(sqlite3_vtab_cursor * cur)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) cur;
	pragmaVtabCursorClear(pCsr);
	sqlite3_free(pCsr);
	return SQLITE_OK;
}
/* Advance the pragma virtual table cursor to the next row */
static int
pragmaVtabNext(sqlite3_vtab_cursor * pVtabCursor)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) pVtabCursor;
	int rc = SQLITE_OK;

	/* Increment the xRowid value */
	pCsr->iRowid++;
	assert(pCsr->pPragma);
	if (SQLITE_ROW != sqlite3_step(pCsr->pPragma)) {
		rc = sqlite3_finalize(pCsr->pPragma);
		pCsr->pPragma = 0;
		pragmaVtabCursorClear(pCsr);
	}
	return rc;
}
/*
** Pragma virtual table module xFilter method.
*/
static int
pragmaVtabFilter(
		 sqlite3_vtab_cursor * pVtabCursor,
		 int idxNum, const char *idxStr,
		 int argc, sqlite3_value ** argv
)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) pVtabCursor;
	PragmaVtab *pTab = (PragmaVtab *) (pVtabCursor->pVtab);
	int rc;
	int i, j;
	StrAccum acc;
	char *zSql;

	UNUSED_PARAMETER(idxNum);
	UNUSED_PARAMETER(idxStr);
	pragmaVtabCursorClear(pCsr);
	j = (pTab->pName->mPragFlg & PragFlg_Result1) != 0 ? 0 : 1;
	for (i = 0; i < argc; i++, j++) {
		assert(j < ArraySize(pCsr->azArg));
		pCsr->azArg[j] = sqlite3_mprintf("%s", sqlite3_value_text(argv[i]));
		if (pCsr->azArg[j] == 0) {
			return SQLITE_NOMEM;
		}
	}
	sqlite3StrAccumInit(&acc, 0, 0, 0, pTab->db->aLimit[SQLITE_LIMIT_SQL_LENGTH]);
	sqlite3StrAccumAppendAll(&acc, "PRAGMA ");
	if (pCsr->azArg[1]) {
		sqlite3XPrintf(&acc, "%Q.", pCsr->azArg[1]);
	}
	sqlite3StrAccumAppendAll(&acc, pTab->pName->zName);
	if (pCsr->azArg[0]) {
		sqlite3XPrintf(&acc, "=%Q", pCsr->azArg[0]);
	}
	zSql = sqlite3StrAccumFinish(&acc);
	if (zSql == 0)
		return SQLITE_NOMEM;
	rc = sqlite3_prepare_v2(pTab->db, zSql, -1, &pCsr->pPragma, 0);
	sqlite3_free(zSql);
	if (rc != SQLITE_OK) {
		pTab->base.zErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(pTab->db));
		return rc;
	}
	return pragmaVtabNext(pVtabCursor);
}
/*
** Pragma virtual table module xEof method.
*/
static int
pragmaVtabEof(sqlite3_vtab_cursor * pVtabCursor)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) pVtabCursor;
	return (pCsr->pPragma == 0);
}
/* The xColumn method simply returns the corresponding column from
** the PRAGMA.
*/
static int
pragmaVtabColumn(
		 sqlite3_vtab_cursor * pVtabCursor,
		 sqlite3_context * ctx,
		 int i
)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) pVtabCursor;
	PragmaVtab *pTab = (PragmaVtab *) (pVtabCursor->pVtab);
	if (i < pTab->iHidden) {
		sqlite3_result_value(ctx, sqlite3_column_value(pCsr->pPragma, i));
	} else {
		sqlite3_result_text(ctx, pCsr->azArg[i - pTab->iHidden], -1, SQLITE_TRANSIENT);
	}
	return SQLITE_OK;
}
/*
** Pragma virtual table module xRowid method.
*/
static int
pragmaVtabRowid(sqlite3_vtab_cursor * pVtabCursor, sqlite_int64 * p)
{
	PragmaVtabCursor *pCsr = (PragmaVtabCursor *) pVtabCursor;
	*p = pCsr->iRowid;
	return SQLITE_OK;
}
/* The pragma virtual table object */
static const sqlite3_module pragmaVtabModule = {
	0,			/* iVersion */
	0,			/* xCreate - create a table */
	pragmaVtabConnect,	/* xConnect - connect to an existing table */
	pragmaVtabBestIndex,	/* xBestIndex - Determine search strategy */
	pragmaVtabDisconnect,	/* xDisconnect - Disconnect from a table */
	0,			/* xDestroy - Drop a table */
	pragmaVtabOpen,		/* xOpen - open a cursor */
	pragmaVtabClose,	/* xClose - close a cursor */
	pragmaVtabFilter,	/* xFilter - configure scan constraints */
	pragmaVtabNext,		/* xNext - advance a cursor */
	pragmaVtabEof,		/* xEof */
	pragmaVtabColumn,	/* xColumn - read data */
	pragmaVtabRowid,	/* xRowid - read data */
	0,			/* xUpdate - write data */
	0,			/* xBegin - begin transaction */
	0,			/* xSync - sync transaction */
	0,			/* xCommit - commit transaction */
	0,			/* xRollback - rollback transaction */
	0,			/* xFindFunction - function overloading */
	0,			/* xRename - rename the table */
	0,			/* xSavepoint */
	0,			/* xRelease */
	0			/* xRollbackTo */
};
/*
** Check to see if zTabName is really the name of a pragma.  If it is,
** then register an eponymous virtual table for that pragma and return
** a pointer to the Module object for the new virtual table.
*/
Module *
sqlite3PragmaVtabRegister(sqlite3 * db, const char *zName)
{
	const PragmaName *pName;
	assert(sqlite3_strnicmp(zName, "pragma_", 7) == 0);
	pName = pragmaLocate(zName + 7);
	if (pName == 0)
		return 0;
	if ((pName->mPragFlg & (PragFlg_Result0 | PragFlg_Result1)) == 0)
		return 0;
	assert(sqlite3HashFind(&db->aModule, zName) == 0);
	return sqlite3VtabCreateModule(db, zName, &pragmaVtabModule, (void *)pName, 0);
}
#endif				/* SQLITE_OMIT_VIRTUALTABLE */

#endif				/* SQLITE_OMIT_PRAGMA */
#endif
