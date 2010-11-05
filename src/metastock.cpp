#include "metastock.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifdef USE_FTS
	#include <fts.h>
#else 
	#include <dirent.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>
#include <fcntl.h>

#include "ms_file.h"
#include "util.h"



#define MAX_FILE_LENGTH (1024*1024)

char Metastock::print_sep = '\t';
unsigned char Metastock::prnt_master_fields = 0;
unsigned char Metastock::prnt_data_fields = 0;
unsigned char Metastock::prnt_data_mr_fields = 0;


Metastock::Metastock() :
	ms_dir(NULL),
	master_name(NULL),
	emaster_name(NULL),
	xmaster_name(NULL),
	ba_master( NULL ),
	master_len(0),
	ba_emaster( NULL ),
	emaster_len(0),
	ba_xmaster( NULL ),
	xmaster_len(0),
	ba_fdat( (char*) malloc( MAX_FILE_LENGTH ) )
{
	error[0] = '\0';
#define MAX_MR_LEN 4096
	mr_len = 0;
	mr_list = (master_record*) calloc( MAX_MR_LEN, sizeof(master_record) );
}



#define SAFE_DELETE( _p_ ) \
	if( _p_ != NULL ) { \
		delete _p_; \
	}


Metastock::~Metastock()
{
	free( ba_fdat );
	free( ba_xmaster );
	free( ba_emaster);
	free( ba_master);
	
	free( mr_list );
	
	free( xmaster_name );
	free( emaster_name );
	free( master_name );
	free( ms_dir );
}


#ifdef USE_FTS

#define CHECK_MASTER( _dst_, _gen_name_ ) \
	if( strcasecmp(_gen_name_, node->fts_name) == 0 ) { \
		assert( _dst_ == NULL ); \
		_dst_ = (char*) malloc( node->fts_namelen + 1 ); \
		strcpy( _dst_, node->fts_name   ); \
	}

bool Metastock::findFiles()
{
	//TODO error handling!
	char *path_argv[] = { ms_dir, NULL };
	FTS *tree = fts_open( path_argv,
		FTS_NOCHDIR | FTS_LOGICAL | FTS_NOSTAT, NULL );
	if (!tree) {
		perror("fts_open");
		assert(false);
	}
	FTSENT *node;
	while ((node = fts_read(tree))) {
		if( (node->fts_level > 0) && (node->fts_info == FTS_D ) ) {
			fts_set(tree, node, FTS_SKIP);
		} else if( node->fts_info == FTS_F ) {
			if( (*node->fts_name == 'F' || *node->fts_name == 'f') &&
				node->fts_name[1] >= '1' && node->fts_name[1] <= '9') {
				char *c_number = node->fts_name + 1;
				char *end;
				long int number = strtol( c_number, &end, 10 );
				assert( number > 0 && number < MAX_MR_LEN && c_number != end );
				if( strcasecmp(end, ".MWD") == 0 || strcasecmp(end, ".DAT") == 0 ) {
					strcpy( mr_list[number].file_name, node->fts_name );
				}
			} else {
				CHECK_MASTER( master_name, "MASTER" );
				CHECK_MASTER( emaster_name, "EMASTER" );
				CHECK_MASTER( xmaster_name, "XMASTER" );
			}
		}
	}
	
	if (errno) {
		perror("fts_read");
		assert( false );
	}
	
	if (fts_close(tree)) {
		perror("fts_close");
		assert( false );
	}
}

#undef CHECK_MASTER

#else /*USE_FTS*/

#define CHECK_MASTER( _dst_, _gen_name_ ) \
	if( strcasecmp(_gen_name_, dirp->d_name) == 0 ) { \
		assert( _dst_ == NULL ); \
		_dst_ = (char*) malloc( strlen(dirp->d_name) + 1 ); \
		strcpy( _dst_, dirp->d_name ); \
	}

bool Metastock::findFiles()
{
	DIR *dirh;
	struct dirent *dirp;
	
	if ((dirh = opendir( ms_dir )) == NULL) {
		setError( ms_dir, strerror(errno) );
		return false;
	}
	
	for (dirp = readdir(dirh); dirp != NULL; dirp = readdir(dirh)) {
		if( ( dirp->d_name[0] == 'F' || dirp->d_name[0] == 'f') &&
			dirp->d_name[1] >= '1' && dirp->d_name[1] <= '9') {
			char *c_number = dirp->d_name + 1;
			char *end;
			long int number = strtol( c_number, &end, 10 );
			assert( number > 0 && number < MAX_MR_LEN && c_number != end );
			if( strcasecmp(end, ".MWD") == 0 || strcasecmp(end, ".DAT") == 0 ) {
				strcpy( mr_list[number].file_name, dirp->d_name );
			}
		} else {
			CHECK_MASTER( master_name, "MASTER" );
			CHECK_MASTER( emaster_name, "EMASTER" );
			CHECK_MASTER( xmaster_name, "XMASTER" );
		}
	}
	
	closedir( dirh );
	return true;
}

#undef CHECK_MASTER

#endif /*USE_FTS*/


bool Metastock::setDir( const char* d )
{
	// set member ms_dir inclusive trailing '/'
	int dir_len = strlen(d);
	ms_dir = (char*) realloc( ms_dir, dir_len + 2 );
	strcpy( ms_dir, d );
	if( ms_dir[ dir_len - 1] != '/' ) {
		ms_dir[dir_len] = '/';
		ms_dir[dir_len + 1] = '\0';
	}
	
	if( !findFiles() ) {
		return false;
	}
	
	if( !readMasters() ){
		return false;
	}
	parseMasters();
	
	return true;
}


bool Metastock::setOutputFormat( char sep, int format )
{
	print_sep = sep;
	if( format < 0 ) {
		setError( "wrong output format" );
		return false;
	}
	prnt_master_fields = format >> 9;
	prnt_data_fields = format;
	prnt_data_mr_fields = format >> 9;
	
	FDat::initPrinter( sep, prnt_data_fields );
	return true;
}


bool Metastock::readFile( const char *file_name , char *buf, int *len ) const
{
	// build file name with full path
	char *file_path = (char*) alloca( strlen(ms_dir) + strlen(file_name) + 1 );
	strcpy( file_path, ms_dir );
	strcpy( file_path + strlen(ms_dir), file_name );
	
	
	int fd = open( file_path, O_RDONLY );
	if( fd < 0 ) {
		setError( file_path, strerror(errno) );
		return false;
	}
	*len = read( fd, buf, MAX_FILE_LENGTH );
	fprintf( stderr, "read %s: %d bytes\n", file_path, *len);
	close( fd );
//	assert( ba->size() == rb ); //TODO
	
	return true;
}


void Metastock::parseMasters()
{
	{
		EMasterFile emf( ba_emaster, emaster_len );
		int cntE = emf.countRecords();
		for( int i = 1; i<=cntE; i++ ) {
			master_record *mr = &mr_list[ emf.fileNumber(i) ];
			assert( mr->record_number == 0 );
			mr_len++;
			emf.getRecord( mr, i );
		}
	}
	
	{
		// NOTE this is just a check EMaster vs. Master
		MasterFile mf( ba_master, master_len );
		int cntM = mf.countRecords();
		for( int i = 1; i<=cntM; i++ ) {
			const master_record *mr = &mr_list[ mf.fileNumber(i) ];
			assert( mr->record_number != 0 );
			mf.getRecord( mr, i );
		}
	}
	
	if( hasXMaster() ) {
		XMasterFile xmf( ba_xmaster, xmaster_len );
		int cntX = xmf.countRecords();
		for( int i = 1; i<=cntX; i++ ) {
			master_record *mr = &mr_list[ xmf.fileNumber(i) ];
			assert( mr->record_number == 0 );
			mr_len++;
			xmf.getRecord( mr, i );
		}
	}
}


bool Metastock::readMasters()
{
	if( master_name != NULL ) {
		ba_master = (char*) malloc( MAX_FILE_LENGTH ); //TODO
		if( !readFile( master_name, ba_master, &master_len ) ) {
			return false;
		}
	} else {
		setError("Master file not found");
		return false;
	}
	
	if( emaster_name != NULL ) {
		ba_emaster = (char*) malloc( MAX_FILE_LENGTH ); //TODO
		if( !readFile( emaster_name, ba_emaster, &emaster_len ) ) {
			return false;
		}
	}else {
		setError("EMaster file not found");
		return false;
	}
	
	if( xmaster_name != NULL ) {
		ba_xmaster = (char*) malloc( MAX_FILE_LENGTH ); //TODO
		if( !readFile( xmaster_name, ba_xmaster,  &xmaster_len ) ) {
			return false;
		}
	} else {
		// xmaster is optional, but we could check for existing f#.mwd >255
	}
	
	return true;
}


const char* Metastock::lastError() const
{
	return error;
}


void Metastock::setError( const char* e1, const char* e2 ) const
{
	if( e2 == NULL || *e2 == '\0' ) {
		snprintf( error, ERROR_LENGTH, "%s", e1);
	} else {
		snprintf( error, ERROR_LENGTH, "%s: %s", e1, e2 );
	}
}


void Metastock::dumpMaster() const
{
	MasterFile mf( ba_master, master_len );
	mf.check();
}


void Metastock::dumpEMaster() const
{
	EMasterFile emf( ba_emaster, emaster_len );
	emf.check();
}


void Metastock::dumpXMaster() const
{
	XMasterFile xmf( ba_xmaster, xmaster_len );
	xmf.check();
}


bool Metastock::dumpSymbolInfo( unsigned short f ) const
{
	char buf[MAX_SIZE_MR_STRING + 1];
	
	if( f!=0 ) {
		if( f > 0 && f < MAX_MR_LEN && mr_list[f].record_number != 0 ) {
			assert( mr_list[f].file_number == f );
			int len = mr_record_to_string( buf, &mr_list[f] );
			buf[len] = '\n';
			buf[len+1] = '\0';
			fputs( buf, stdout );
		} else {
			setError("data file not referenced by master files");
			return false;
		}
		return true;
	}
	
	for( int i = 1; i<MAX_MR_LEN; i++ ) {
		if( i > 0 && i < MAX_MR_LEN && mr_list[i].record_number != 0 ) {
			assert( mr_list[i].file_number == i );
			int len = mr_record_to_string( buf, &mr_list[i] );
			buf[len] = '\n';
			buf[len+1] = '\0';
			fputs( buf, stdout );
		}
	}
	return true;
}


int Metastock::build_mr_string( char *dst, const master_record *mr ) const
{
	char *cp = dst;
	int tmp = 0;
	
	if( prnt_data_mr_fields & M_SYM ) {
		tmp = strlen(mr->c_symbol);
		memcpy( cp, mr->c_symbol, tmp );
		cp += tmp;
		*cp++ = print_sep;
	}
	
	if( prnt_data_mr_fields & M_NAM ) {
		tmp = strlen(mr->c_long_name);
		memcpy( cp, mr->c_long_name, tmp );
		cp += tmp;
		*cp++ = print_sep;
	}
	
	*cp = '\0';
	return cp - dst;
}


bool Metastock::dumpData( unsigned short f ) const
{
	char buf[256];
	
	if( f!=0 ) {
		if( f > 0 && f < MAX_MR_LEN && mr_list[f].record_number != 0 ) {
			assert( mr_list[f].file_number == f );
			int len = build_mr_string( buf, &mr_list[f] );
			if( !dumpData( f, mr_list[f].field_bitset, buf ) ) {
				return false;
			}
		} else {
			setError("data file not referenced by master files");
			return false;
		}
		return true;
	}
	
	for( int i = 1; i<MAX_MR_LEN; i++ ) {
		if( i > 0 && i < MAX_MR_LEN && mr_list[i].record_number != 0 ) {
			assert( mr_list[i].file_number == i );
			int len = build_mr_string( buf, &mr_list[i] );
			if( !dumpData( i, mr_list[i].field_bitset, buf ) ) {
				return false;
			}
		}
	}
	return true;
}



bool Metastock::dumpData( unsigned short n, unsigned char fields, const char *pfx ) const
{
	const char *fdat_name = mr_list[n].file_name;
	
	if( *fdat_name == '\0' ) {
		setError( "no fdat found" );
		return false;
	}
	
	int fdat_len = 0;
	if( ! readFile( fdat_name, ba_fdat, &fdat_len ) ) {
		return false;
	}
	
	FDat datfile( ba_fdat, fdat_len, fields );
	fprintf( stderr, "#%d: %d x %d bytes\n",
		n, datfile.countRecords(), count_bits(fields) * 4 );
	
	datfile.checkHeader();
	datfile.print( pfx );
	
	return true;
}


bool Metastock::hasXMaster() const
{
	return( xmaster_name != NULL  );
}
