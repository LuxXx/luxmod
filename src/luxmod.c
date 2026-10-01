/*
 * luxmod - runtime weapon config loader grafted into UrT 4.3.4 qagame.qvm
 *
 * by LuxXx - https://github.com/LuxXx - https://x.com/luxdav
 *
 * Loads a config (cvar lux_config, default "luxmod.cfg") on every
 * GAME_INIT and patches the qagame weapon table in place.  The original table
 * is backed up once, so every load starts from stock values.
 *
 * Server console / rcon commands:
 *   lux_reload          re-read the config now
 *   lux_dump <weapon>   print the current values of a weapon
 *   lux_diff            list every table field that differs from stock
 *   lux_throw <player> <he|smoke|hk69>  make a player fire a projectile
 *   gh <player|all> <hp>       set health (+N / -N adds)
 *   gw <player|all> <weapons>  give or refill weapons: name ("lr300") or letters
 *   gi <player|all> <items>    give items: name ("medkit") or letters a-g
 *
 * Built with q3lcc and grafted by tools/build_luxmod.py; host symbols
 * (G_InitGame, ConsoleCommand, lux_blob_init) are resolved through equ.
 */

/* ---- host / engine interface ------------------------------------------ */

void	trap_Printf( const char *s );
int	trap_FS_FOpenFile( const char *qpath, int *f, int mode );
void	trap_FS_Read( void *buffer, int len, int f );
void	trap_FS_FCloseFile( int f );
int	trap_Argc( void );
void	trap_Argv( int n, char *buffer, int bufferLength );
void	trap_Cvar_VariableStringBuffer( const char *name, char *buffer, int len );

void	G_InitGame( int levelTime, int randomSeed, int restart );
int	ConsoleCommand( void );
void	*UT_ClientFromString( const char *s );	/* prints its own errors, 0 if none */
int	UT_GiveWeapon( void *client, int weapon, int mode );	/* slot or -1 */
int	UT_FindWeaponSlot( void *client, int weapon );	/* weapon 0 = free slot */
int	UT_GiveItem( void *client, int item );	/* slot or -1 */
int	G_Damage( int targ, int inflictor, int attacker, float *dir, float *point,
		int damage, int dflags, int mod, int extra );
void	G_Knockback( int targ, float *dir, float knockback );
void	UT_FireHK69( int ent );
void	UT_FireGrenade( int ent );	/* HE */
void	UT_FireWeapon( int ent );	/* sets up aim vectors, dispatches on s.weapon */
void	UT_ClientSpawn( int ent );
void	UT_FireSmoke( int ent );
float	sqrt( float x );
void	lux_blob_init( void );	/* generated: writes our DATA/LIT into memory */

/* ---- weapon table layout (4.3.4) -------------------------------------- */

#define WT_BASE		0xfb4
#define WT_STRIDE	432
#define WT_COUNT	28
#define WT_CLIPS	28	/* "Shells": spare magazines */
#define WT_AMMO		32	/* magazine size */
#define WT_RELOAD	36	/* reload time, ms */
#define WT_RANGE	168	/* damage falloff distance / 10; stock code uses it for the SPAS only */
#define WT_KNOCKBACK	172	/* float, bullet knockback (capped at 200) */
#define WT_MOD		180	/* means of death */
#define WT_HITLOC	48	/* NUM_HITLOCS x { float damage; int bleeds; } */
#define WT_MODES	224	/* WT_NUM_MODES x 52-byte fire modes */
#define WT_NUM_MODES	4
#define MODE_STRIDE	52
#define MODE_TYPE	4	/* 0/1 hitscan auto/single, 17+ projectile kinds */
#define MODE_CYCLE	8	/* ms between shots */
#define MODE_SPEED	36	/* float: projectile speed, or spread at full heat for bullets */

/* ---- player structures (4.3.4) --------------------------------------- */

#define LEVEL_CLIENTS	0x215f170	/* gclient_t *level.clients */
#define LEVEL_MAXCLIENTS 0x215f18c
#define G_ENTITIES	0x2083168
#define CLIENT_SIZE	6336
#define ENTITY_SIZE	880
#define ENT_HEALTH	684
#define CL_HEALTH	208	/* ps.stats[STAT_HEALTH] */
#define CL_INVENTORY	312	/* 16 x { u8 weapon/item, ammo, mode, clips } */
#define CL_CONNECTED	468	/* 2 = in game */
#define CL_WEAPMODES	668	/* char per weapon id, '0' + fire mode */
#define CL_TEAM		3096	/* 3 = spectator */
#define CL_GHOST	4324	/* nonzero while dead */
#define LEVEL_TIME	0x215f194
#define G_GAMETYPE	0x2082f24
#define MAX_GENTITIES	1024
#define ENT_WEAPON	192	/* s.weapon */
#define ENT_PARENT	516
#define ENT_INUSE	532
#define ENT_NEXTTHINK	644
#define ENT_TAKEDAMAGE	688
#define ENT_DAMAGE	692	/* impact */
#define ENT_SPLASHDMG	696
#define ENT_SPLASHRAD	700
#define DAMAGE_NO_KNOCKBACK 4
#define FIRST_ITEM	17	/* bg_itemlist: Vest, NVG, Medkit, Silencer, Laser, Helmet, Extra Ammo */
#define NUM_ITEMS	7

#define NUM_HITLOCS	15	/* slot 0 is not a body part; config uses 1..14 */

#define CFG_MAX		65536
#define MAX_DEPTH	8

static const char *hitlocNames[NUM_HITLOCS] = {
	"", "Head", "Helmet", "Torso", "Vest", "Left Arm", "Right Arm", "Groin",
	"Butt", "Left Upper Leg", "Right Upper Leg", "Left Lower Leg",
	"Right Lower Leg", "Left Foot", "Right Foot"
};

static unsigned char	luxBackup[WT_COUNT * WT_STRIDE];
static int		luxBackedUp;
static char		cfgBuf[CFG_MAX + 1];

/* pending per-weapon damage settings, applied after parsing */
static int	dmgPercent[WT_COUNT];
static float	locDamage[WT_COUNT][NUM_HITLOCS];
static int	locSet[WT_COUNT][NUM_HITLOCS];
static int	locBleed[WT_COUNT][NUM_HITLOCS];	/* -1 keep, 0/1 set */

static int	numApplied, numIgnored, numErrors;

/* explosives (defined further down) */
typedef struct {
	int	fuse, radius, splash, impact;
	int	knock, self;
} luxProj_t;
static luxProj_t	projCfg[WT_COUNT];	/* -1 = stock; knock/self in percent */

/* read by stock functions that build_luxmod.py copies with these patched in */
int	lux_heal_limit = 50;
int	lux_heal_limit_medkit = 90;
int	lux_heal_step = 15;
int	lux_bandage_time = 1500;
int	lux_bandage_time_medkit = 750;
float	lux_fall_injury_far = 0.6f;
float	lux_fall_injury_medium = 0.3f;

/* Player { } settings */
#define BLEED_TABLE	29088	/* int[5]: 100 ms ticks per HP lost, by wound count */
#define NUM_BLEED	5
static const int bleedStock[NUM_BLEED] = { 9, 4, 3, 2, 1 };
static int	startHealth;
static int	luxReady;	/* hooks pass through until the first Lux_Load */
static int	causeScale[64];	/* Damage { } percent by means of death */

typedef struct {
	const char	*name;
	int		mod;
} luxCause_t;

static const luxCause_t causes[] = {
	{ "Falling", 6 }, { "Bleeding", 23 }, { "Kick", 24 }, { "Goomba", 48 },
	{ "Drowning", 1 }, { "Slime", 2 }, { "Lava", 3 }, { "Crush", 4 },
	{ "Trigger Hurt", 9 }, { "Slap", 32 }, { 0, 0 }
};
static int Lux_IsExplosive( int w );
static void Lux_ResetProjectiles( void );
static int	verbose;

/* ---- tiny libc -------------------------------------------------------- */

static int Lux_Lower( int c ) {
	return ( c >= 'A' && c <= 'Z' ) ? c + 32 : c;
}

/* compare ignoring case, spaces and punctuation: "Semi-Automatic" == "semi automatic" */
static int Lux_NameEq( const char *a, const char *b ) {
	for ( ;; ) {
		while ( *a && !( ( *a >= '0' && *a <= '9' ) || ( Lux_Lower( *a ) >= 'a' && Lux_Lower( *a ) <= 'z' ) ) ) a++;
		while ( *b && !( ( *b >= '0' && *b <= '9' ) || ( Lux_Lower( *b ) >= 'a' && Lux_Lower( *b ) <= 'z' ) ) ) b++;
		if ( Lux_Lower( *a ) != Lux_Lower( *b ) ) return 0;
		if ( !*a ) return 1;
		a++; b++;
	}
}

static void Lux_Cat( char *dst, int size, const char *src ) {
	int n = 0;
	while ( dst[n] ) n++;
	while ( *src && n < size - 1 ) dst[n++] = *src++;
	dst[n] = 0;
}

static void Lux_CatInt( char *dst, int size, int v ) {
	char tmp[16];
	int i = 15, neg = v < 0;
	unsigned u = neg ? -v : v;
	tmp[i] = 0;
	do { tmp[--i] = '0' + u % 10; u /= 10; } while ( u );
	if ( neg ) tmp[--i] = '-';
	Lux_Cat( dst, size, tmp + i );
}

static void Lux_CatFloat( char *dst, int size, float f ) {
	int whole, frac;
	if ( f < 0 ) { Lux_Cat( dst, size, "-" ); f = -f; }
	whole = (int)f;
	frac = (int)( ( f - whole ) * 100 + 0.5f );
	if ( frac >= 100 ) { whole++; frac -= 100; }
	Lux_CatInt( dst, size, whole );
	Lux_Cat( dst, size, "." );
	if ( frac < 10 ) Lux_Cat( dst, size, "0" );
	Lux_CatInt( dst, size, frac );
}

/* exact for up to 7 significant digits: integer mantissa, one division */
static int Lux_ParseNum( const char *s, float *out ) {
	int mant = 0, digits = 0, frac = 0, neg = 0, i;
	float div = 1;

	while ( *s == ' ' || *s == '\t' ) s++;
	if ( *s == '-' ) { neg = 1; s++; } else if ( *s == '+' ) s++;
	while ( *s >= '0' && *s <= '9' ) {
		if ( mant < 100000000 ) mant = mant * 10 + ( *s - '0' ); else frac--;
		s++; digits++;
	}
	if ( *s == '.' ) {
		s++;
		while ( *s >= '0' && *s <= '9' ) {
			if ( mant < 100000000 ) { mant = mant * 10 + ( *s - '0' ); frac++; }
			s++; digits++;
		}
	}
	if ( !digits ) return 0;
	for ( i = 0; i < frac; i++ ) div *= 10;
	*out = (float)mant / div;
	for ( i = 0; i > frac; i-- ) *out *= 10;
	if ( neg ) *out = -*out;
	return 1;
}

static void Lux_Print2( const char *a, const char *b ) {
	char line[256];
	line[0] = 0;
	Lux_Cat( line, sizeof( line ), a );
	Lux_Cat( line, sizeof( line ), b );
	Lux_Cat( line, sizeof( line ), "\n" );
	trap_Printf( line );
}

/* ---- table access ----------------------------------------------------- */

static unsigned char *Lux_Weapon( int w ) {
	return (unsigned char *)( WT_BASE + w * WT_STRIDE );
}

static int *Lux_Int( int w, int off ) {
	return (int *)( Lux_Weapon( w ) + off );
}

static float *Lux_Float( int w, int off ) {
	return (float *)( Lux_Weapon( w ) + off );
}

static const char *Lux_WeaponName( int w ) {
	return *(const char **)Lux_Weapon( w );
}

static int Lux_FindWeapon( const char *name ) {
	int w;
	for ( w = 1; w < WT_COUNT; w++ ) {
		if ( Lux_WeaponName( w ) && Lux_NameEq( Lux_WeaponName( w ), name ) ) return w;
	}
	return 0;
}

/* returns byte offset of the fire mode in the weapon, or -1 */
static int Lux_FindMode( int w, const char *name ) {
	int m;
	const char *mn;
	for ( m = 0; m < WT_NUM_MODES; m++ ) {
		mn = *(const char **)( Lux_Weapon( w ) + WT_MODES + m * MODE_STRIDE );
		if ( mn && Lux_NameEq( mn, name ) ) return WT_MODES + m * MODE_STRIDE;
	}
	return -1;
}

/* HK69 (17/19), grenades (24) and the thrown knife (26) fire projectiles;
   for hitscan modes the MODE_SPEED slot holds something else */
static int Lux_IsProjectile( int w, int mode ) {
	int t = *Lux_Int( w, mode + MODE_TYPE );
	return t == 17 || t == 19 || t == 24 || t == 26;
}

static int Lux_IsHitscan( int w, int mode ) {
	int t = *Lux_Int( w, mode + MODE_TYPE );
	return t == 0 || t == 1;
}

/* config keys that map straight onto a table field */
#define F_INT		0
#define F_FLOAT		1
#define K_ANY		0	/* any fire mode */
#define K_HITSCAN	1	/* bullet modes only */
#define K_PROJ		2	/* grenades, HK69, thrown knife only */

typedef struct {
	const char	*name;
	int		off;
	int		type;
	int		kind;	/* mode fields only */
} luxField_t;

static const luxField_t weaponFields[] = {
	{ "Ammo", WT_AMMO, F_INT, 0 },
	{ "Shells", WT_CLIPS, F_INT, 0 },
	{ "Reload Time", WT_RELOAD, F_INT, 0 },
	{ "Knockback", WT_KNOCKBACK, F_FLOAT, 0 },
	{ "Range", WT_RANGE, F_INT, 0 },
	{ 0, 0, 0, 0 }
};

static const luxField_t modeFields[] = {
	{ "Cycle", MODE_CYCLE, F_INT, K_ANY },
	{ "Speed", MODE_SPEED, F_FLOAT, K_PROJ },
	{ "Spread", 16, F_FLOAT, K_HITSCAN },
	{ "Moving Spread", 20, F_FLOAT, K_HITSCAN },
	{ "Heat Delay", 24, F_INT, K_HITSCAN },
	{ "Heat Cooldown", 28, F_INT, K_HITSCAN },
	{ "Vertical Spread", 32, F_FLOAT, K_HITSCAN },
	{ "Heat Spread", MODE_SPEED, F_FLOAT, K_HITSCAN },
	{ "Heat Factor", 40, F_FLOAT, K_HITSCAN },
	{ "Burst Cycle", 44, F_INT, K_HITSCAN },
	{ "Burst Shots", 48, F_INT, K_HITSCAN },
	{ 0, 0, 0, 0 }
};

/* explosives: offsets into luxProj_t, applied to each spawned projectile */
static const luxField_t projFields[] = {
	{ "Fuse", 0, F_INT, 0 },
	{ "Splash Radius", 4, F_INT, 0 },
	{ "Splash Damage", 8, F_INT, 0 },
	{ "Impact Damage", 12, F_INT, 0 },
	{ "Splash Knockback", 16, F_INT, 0 },
	{ "Self Damage", 20, F_INT, 0 },
	{ 0, 0, 0, 0 }
};

static const luxField_t *Lux_FindField( const luxField_t *t, const char *key ) {
	for ( ; t->name; t++ ) {
		if ( Lux_NameEq( t->name, key ) ) return t;
	}
	return 0;
}

static int Lux_FieldFits( int w, int mode, const luxField_t *fd ) {
	if ( fd->kind == K_HITSCAN ) return Lux_IsHitscan( w, mode );
	if ( fd->kind == K_PROJ ) return Lux_IsProjectile( w, mode );
	return 1;
}

static void Lux_StoreField( int w, int off, const luxField_t *fd, float f ) {
	if ( fd->type == F_FLOAT ) *Lux_Float( w, off ) = f;
	else *Lux_Int( w, off ) = (int)f;
}

static int Lux_FindHitloc( const char *name ) {
	int i;
	for ( i = 1; i < NUM_HITLOCS; i++ ) {
		if ( Lux_NameEq( hitlocNames[i], name ) ) return i;
	}
	return 0;
}

static void Lux_Restore( void ) {
	unsigned char *t = Lux_Weapon( 0 );
	int i;
	if ( !luxBackedUp ) {
		for ( i = 0; i < WT_COUNT * WT_STRIDE; i++ ) luxBackup[i] = t[i];
		luxBackedUp = 1;
	} else {
		for ( i = 0; i < WT_COUNT * WT_STRIDE; i++ ) t[i] = luxBackup[i];
	}
}

/* ---- config parsing --------------------------------------------------- */

static char	scope[MAX_DEPTH][64];
static int	depth;
static int	lineNum;

static void Lux_Warn( const char *msg, const char *what ) {
	char line[256];
	line[0] = 0;
	Lux_Cat( line, sizeof( line ), "^3luxmod: line " );
	Lux_CatInt( line, sizeof( line ), lineNum );
	Lux_Cat( line, sizeof( line ), ": " );
	Lux_Cat( line, sizeof( line ), msg );
	Lux_Cat( line, sizeof( line ), what );
	Lux_Cat( line, sizeof( line ), "\n" );
	trap_Printf( line );
}

static void Lux_Ignore( const char *key ) {
	numIgnored++;
	if ( verbose ) Lux_Warn( "unsupported, ignored: ", key );
}

static void Lux_SetWeaponKey( int w, int mode, const char *key, const char *val ) {
	const luxField_t *fd;
	float f;
	int m, off, any;

	if ( !Lux_ParseNum( val, &f ) ) {
		/* empty values mean "keep default" */
		while ( *val == ' ' ) val++;
		if ( *val ) { Lux_Warn( "not a number: ", val ); numErrors++; }
		return;
	}

	if ( Lux_NameEq( key, "Damage" ) ) {
		/* "Damage" on explosives would be impact damage, not a percent: skip */
		if ( Lux_IsProjectile( w, WT_MODES ) && !Lux_NameEq( Lux_WeaponName( w ), "Knife" ) ) {
			Lux_Ignore( key );
			return;
		}
		dmgPercent[w] = (int)f;	/* knife Slash/Throw blocks: whole weapon */
		numApplied++;
		return;
	}

	if ( mode < 0 && ( fd = Lux_FindField( projFields, key ) ) != 0 ) {
		if ( !Lux_IsExplosive( w ) ) {
			Lux_Warn( "only for HK69 and grenades, ignored: ", key );
			numErrors++;
			return;
		}
		*(int *)( (char *)&projCfg[w] + fd->off ) = (int)f;
		numApplied++;
		return;
	}

	if ( mode < 0 && ( fd = Lux_FindField( weaponFields, key ) ) != 0 ) {
		Lux_StoreField( w, fd->off, fd, f );
		numApplied++;
		return;
	}

	fd = Lux_FindField( modeFields, key );
	/* "Heat Spread" and "Speed" share a slot: pick by mode kind below */
	if ( !fd ) {
		Lux_Ignore( key );
		return;
	}

	if ( mode >= 0 ) {
		if ( !Lux_FieldFits( w, mode, fd ) ) {
			Lux_Warn( fd->kind == K_PROJ ? "only for grenades/HK69/thrown knife, ignored: "
				: "only for bullet fire modes, ignored: ", key );
			numErrors++;
			return;
		}
		Lux_StoreField( w, mode + fd->off, fd, f );
		numApplied++;
		return;
	}

	/* weapon level: every fire mode it applies to */
	any = 0;
	for ( m = 0; m < WT_NUM_MODES; m++ ) {
		off = WT_MODES + m * MODE_STRIDE;
		if ( !*(int *)( Lux_Weapon( w ) + off ) || !Lux_FieldFits( w, off, fd ) ) continue;
		Lux_StoreField( w, off + fd->off, fd, f );
		any = 1;
	}
	if ( any ) numApplied++;
	else { Lux_Warn( "weapon has no fire mode this applies to: ", key ); numErrors++; }
}

static void Lux_SetHitloc( int w, const char *key, const char *val ) {
	int loc = Lux_FindHitloc( key );
	float f;
	const char *p;

	if ( !loc ) { Lux_Warn( "unknown hit location: ", key ); numErrors++; return; }
	if ( !Lux_ParseNum( val, &f ) ) { Lux_Warn( "not a number: ", val ); numErrors++; return; }
	locDamage[w][loc] = f;
	locSet[w][loc] = 1;
	/* optional "bleed" / "nobleed" after the number */
	for ( p = val; *p; p++ ) {
		if ( Lux_Lower( p[0] ) == 'n' && Lux_Lower( p[1] ) == 'o' && Lux_Lower( p[2] ) == 'b' ) {
			locBleed[w][loc] = 0;
			break;
		}
		if ( Lux_Lower( p[0] ) == 'b' && Lux_Lower( p[1] ) == 'l' ) {
			locBleed[w][loc] = 1;
			break;
		}
	}
	numApplied++;
}

static void Lux_SetPlayerKey( const char *key, const char *val ) {
	float f;
	int i, v;

	if ( !Lux_ParseNum( val, &f ) ) {
		while ( *val == ' ' ) val++;
		if ( *val ) { Lux_Warn( "not a number: ", val ); numErrors++; }
		return;
	}
	v = (int)f;
	if ( Lux_NameEq( key, "Start Health" ) ) {
		if ( v < 1 || v > 100 ) { Lux_Warn( "Start Health must be 1..100", "" ); numErrors++; return; }
		startHealth = v;
	} else if ( Lux_NameEq( key, "Heal Limit" ) ) {
		lux_heal_limit = v;
	} else if ( Lux_NameEq( key, "Heal Limit Medkit" ) ) {
		lux_heal_limit_medkit = v;
	} else if ( Lux_NameEq( key, "Heal Step" ) ) {
		lux_heal_step = v;
	} else if ( Lux_NameEq( key, "Bandage Time" ) ) {
		lux_bandage_time = v;
	} else if ( Lux_NameEq( key, "Bandage Time Medkit" ) ) {
		lux_bandage_time_medkit = v;
	} else if ( Lux_NameEq( key, "Fall Injury" ) ) {
		lux_fall_injury_far = 0.6f * f / 100;
		lux_fall_injury_medium = 0.3f * f / 100;
	} else if ( Lux_NameEq( key, "Bleed Speed" ) ) {
		if ( f <= 0 ) { Lux_Warn( "Bleed Speed must be > 0 (use Damage { Bleeding: 0 } to disable)", "" ); numErrors++; return; }
		for ( i = 0; i < NUM_BLEED; i++ ) {
			v = (int)( bleedStock[i] * 100 / f + 0.5f );
			( (int *)BLEED_TABLE )[i] = v < 1 ? 1 : v;	/* game does % on it: never 0 */
		}
	} else {
		Lux_Ignore( key );
		return;
	}
	numApplied++;
}

static void Lux_SetCause( const char *key, const char *val ) {
	const luxCause_t *c;
	float f;

	for ( c = causes; c->name; c++ ) {
		if ( Lux_NameEq( c->name, key ) ) break;
	}
	if ( !c->name ) { Lux_Ignore( key ); return; }
	if ( !Lux_ParseNum( val, &f ) || f < 0 ) { Lux_Warn( "not a percentage: ", val ); numErrors++; return; }
	causeScale[c->mod] = (int)f;
	numApplied++;
}

static void Lux_Assign( const char *key, const char *val ) {
	int w, mode;

	if ( depth == 1 && Lux_NameEq( scope[0], "Player" ) ) {
		Lux_SetPlayerKey( key, val );
		return;
	}
	if ( depth == 1 && Lux_NameEq( scope[0], "Damage" ) ) {
		Lux_SetCause( key, val );
		return;
	}

	if ( depth >= 2 && Lux_NameEq( scope[0], "Weapons" ) && !Lux_NameEq( scope[1], "Kick" ) ) {
		w = Lux_FindWeapon( scope[1] );
		if ( !w ) return;	/* already warned when the block opened */
		if ( depth == 2 ) {
			Lux_SetWeaponKey( w, -1, key, val );
			return;
		}
		if ( depth == 3 && Lux_NameEq( scope[2], "Hit Locations" ) ) {
			Lux_SetHitloc( w, key, val );
			return;
		}
		if ( depth == 3 ) {
			mode = Lux_FindMode( w, scope[2] );
			if ( mode < 0 ) { Lux_Warn( "no such fire mode: ", scope[2] ); numErrors++; return; }
			Lux_SetWeaponKey( w, mode, key, val );
			return;
		}
	}
	Lux_Ignore( key );
}

static void Lux_Open( const char *name ) {
	if ( depth >= MAX_DEPTH ) { Lux_Warn( "blocks nested too deep", "" ); numErrors++; return; }
	scope[depth][0] = 0;
	Lux_Cat( scope[depth], sizeof( scope[0] ), name );
	depth++;
	/* "Kick" is a boot settings block, its keys count as unsupported */
	if ( depth == 2 && Lux_NameEq( scope[0], "Weapons" ) && !Lux_NameEq( name, "Kick" ) && !Lux_FindWeapon( name ) ) {
		Lux_Warn( "unknown weapon: ", name );
		numErrors++;
	}
}

/* strip comment and surrounding blanks in place, return start */
static char *Lux_Trim( char *s ) {
	char *e, *c;
	for ( c = s; *c; c++ ) {
		if ( c[0] == '/' && c[1] == '/' ) { *c = 0; break; }
	}
	while ( *s == ' ' || *s == '\t' || *s == '\r' ) s++;
	e = s;
	while ( *e ) e++;
	while ( e > s && ( e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ) ) *--e = 0;
	return s;
}

/* "Name { Key: 1 }" -> "Name {" / "Key: 1" / "}" so the line parser sees one item per line */
static char cfgNorm[CFG_MAX * 2 + 1];

static char *Lux_SplitBraces( const char *in ) {
	char *o = cfgNorm, *end = cfgNorm + sizeof( cfgNorm ) - 3;
	for ( ; *in && o < end; in++ ) {
		if ( *in == '/' && in[1] == '/' ) {	/* keep comments intact */
			while ( *in && *in != '\n' && o < end ) *o++ = *in++;
			if ( !*in ) break;
		}
		if ( *in == '}' ) *o++ = '\n';
		*o++ = *in;
		if ( *in == '{' || *in == '}' ) *o++ = '\n';
	}
	*o = 0;
	return cfgNorm;
}

static void Lux_Parse( char *text ) {
	char pending[64];
	char *line, *next, *colon, *brace;

	pending[0] = 0;
	depth = 0;
	lineNum = 0;
	for ( line = text; line && *line; line = next ) {
		next = line;
		while ( *next && *next != '\n' ) next++;
		if ( *next ) *next++ = 0; else next = 0;
		lineNum++;

		line = Lux_Trim( line );
		if ( !*line ) continue;

		if ( line[0] == '}' ) {
			if ( depth > 0 ) depth--;
			else { Lux_Warn( "unmatched }", "" ); numErrors++; }
			continue;
		}
		if ( line[0] == '{' ) {
			if ( !pending[0] ) { Lux_Warn( "block without a name", "" ); numErrors++; }
			Lux_Open( pending );
			pending[0] = 0;
			continue;
		}
		brace = line;
		while ( *brace ) brace++;
		if ( brace[-1] == '{' ) {
			brace[-1] = 0;
			Lux_Open( Lux_Trim( line ) );
			pending[0] = 0;
			continue;
		}
		colon = line;
		while ( *colon && *colon != ':' ) colon++;
		if ( *colon ) {
			*colon = 0;
			Lux_Assign( Lux_Trim( line ), colon + 1 );
			pending[0] = 0;
			continue;
		}
		/* bare name: block name with "{" on the next line */
		pending[0] = 0;
		Lux_Cat( pending, sizeof( pending ), line );
	}
	if ( depth ) { Lux_Warn( "missing } at end of file", "" ); numErrors++; }
}

static void Lux_ApplyDamage( void ) {
	int w, loc;
	float *dmg;

	for ( w = 1; w < WT_COUNT; w++ ) {
		for ( loc = 1; loc < NUM_HITLOCS; loc++ ) {
			dmg = Lux_Float( w, WT_HITLOC + loc * 8 );
			if ( locSet[w][loc] ) *dmg = locDamage[w][loc] / 100.0f;
			if ( dmgPercent[w] != 100 ) *dmg = *dmg * dmgPercent[w] / 100.0f;
			if ( locBleed[w][loc] >= 0 ) *Lux_Int( w, WT_HITLOC + loc * 8 + 4 ) = locBleed[w][loc];
		}
	}
}

static void Lux_ResetPlayer( void ) {
	int i;
	startHealth = 100;
	lux_heal_limit = 50;
	lux_heal_limit_medkit = 90;
	lux_heal_step = 15;
	lux_bandage_time = 1500;
	lux_bandage_time_medkit = 750;
	lux_fall_injury_far = 0.6f;
	lux_fall_injury_medium = 0.3f;
	for ( i = 0; i < NUM_BLEED; i++ ) ( (int *)BLEED_TABLE )[i] = bleedStock[i];
	for ( i = 0; i < 64; i++ ) causeScale[i] = 100;
	luxReady = 1;
}

static void Lux_Load( void ) {
	char path[64], tmp[16], msg[160];
	int f, len, w, loc;

	trap_Cvar_VariableStringBuffer( "lux_verbose", tmp, sizeof( tmp ) );
	verbose = tmp[0] - '0';
	if ( verbose < 0 || verbose > 9 ) verbose = 0;
	trap_Cvar_VariableStringBuffer( "lux_config", path, sizeof( path ) );
	if ( !path[0] ) {
		path[0] = 0;
		Lux_Cat( path, sizeof( path ), "luxmod.cfg" );
	}

	Lux_Restore();
	for ( w = 0; w < WT_COUNT; w++ ) {
		dmgPercent[w] = 100;
		for ( loc = 0; loc < NUM_HITLOCS; loc++ ) {
			locSet[w][loc] = 0;
			locBleed[w][loc] = -1;
		}
	}
	numApplied = numIgnored = numErrors = 0;
	Lux_ResetProjectiles();
	Lux_ResetPlayer();

	len = trap_FS_FOpenFile( path, &f, 0 );
	if ( !f || len <= 0 ) {
		Lux_Print2( "luxmod: no config, using stock values: ", path );
		if ( f ) trap_FS_FCloseFile( f );
		return;
	}
	if ( len > CFG_MAX ) {
		Lux_Print2( "^1luxmod: config too large (64k max): ", path );
		trap_FS_FCloseFile( f );
		return;
	}
	trap_FS_Read( cfgBuf, len, f );
	trap_FS_FCloseFile( f );
	cfgBuf[len] = 0;

	Lux_Parse( Lux_SplitBraces( cfgBuf ) );
	Lux_ApplyDamage();

	msg[0] = 0;
	Lux_Cat( msg, sizeof( msg ), "luxmod: loaded " );
	Lux_Cat( msg, sizeof( msg ), path );
	Lux_Cat( msg, sizeof( msg ), ": " );
	Lux_CatInt( msg, sizeof( msg ), numApplied );
	Lux_Cat( msg, sizeof( msg ), " applied, " );
	Lux_CatInt( msg, sizeof( msg ), numIgnored );
	Lux_Cat( msg, sizeof( msg ), " unsupported (set lux_verbose 1 to list), " );
	Lux_CatInt( msg, sizeof( msg ), numErrors );
	Lux_Cat( msg, sizeof( msg ), " errors\n" );
	trap_Printf( msg );
}

/* "lr300" finds "ZM LR300": unique case-insensitive substring of the name */
static int Lux_FindWeaponLoose( const char *query ) {
	char q[64], n[64];
	int w, found = 0, i, j;
	const char *s;

	for ( i = 0, s = query; *s && i < 63; s++ ) {
		if ( ( *s >= '0' && *s <= '9' ) || ( Lux_Lower( *s ) >= 'a' && Lux_Lower( *s ) <= 'z' ) ) q[i++] = Lux_Lower( *s );
	}
	q[i] = 0;
	if ( !q[0] ) return 0;
	for ( w = 1; w < WT_COUNT; w++ ) {
		if ( !Lux_WeaponName( w ) ) continue;
		for ( i = 0, s = Lux_WeaponName( w ); *s && i < 63; s++ ) {
			if ( ( *s >= '0' && *s <= '9' ) || ( Lux_Lower( *s ) >= 'a' && Lux_Lower( *s ) <= 'z' ) ) n[i++] = Lux_Lower( *s );
		}
		n[i] = 0;
		for ( i = 0; n[i]; i++ ) {
			for ( j = 0; q[j] && n[i + j] == q[j]; j++ ) ;
			if ( !q[j] ) {
				if ( found && found != w ) return 0;	/* ambiguous */
				found = w;
				break;
			}
		}
	}
	return found;
}

static void Lux_DumpPlayer( void ) {
	char line[512];
	const luxCause_t *c;
	int i;

	line[0] = 0;
	Lux_Cat( line, sizeof( line ), "Player: Start Health " );
	Lux_CatInt( line, sizeof( line ), startHealth );
	Lux_Cat( line, sizeof( line ), ", Heal Limit " );
	Lux_CatInt( line, sizeof( line ), lux_heal_limit );
	Lux_Cat( line, sizeof( line ), ", Heal Limit Medkit " );
	Lux_CatInt( line, sizeof( line ), lux_heal_limit_medkit );
	Lux_Cat( line, sizeof( line ), ", Heal Step " );
	Lux_CatInt( line, sizeof( line ), lux_heal_step );
	Lux_Cat( line, sizeof( line ), ", Bandage Time " );
	Lux_CatInt( line, sizeof( line ), lux_bandage_time );
	Lux_Cat( line, sizeof( line ), ", Bandage Time Medkit " );
	Lux_CatInt( line, sizeof( line ), lux_bandage_time_medkit );
	Lux_Cat( line, sizeof( line ), ", Fall Injury " );
	Lux_CatInt( line, sizeof( line ), (int)( lux_fall_injury_far / 0.6f * 100 + 0.5f ) );
	Lux_Cat( line, sizeof( line ), "%\n  bleed ticks (100ms) per HP by wounds:" );
	for ( i = 0; i < NUM_BLEED; i++ ) {
		Lux_Cat( line, sizeof( line ), " " );
		Lux_CatInt( line, sizeof( line ), ( (int *)BLEED_TABLE )[i] );
	}
	Lux_Cat( line, sizeof( line ), "\nDamage:" );
	for ( c = causes; c->name; c++ ) {
		Lux_Cat( line, sizeof( line ), " " );
		Lux_Cat( line, sizeof( line ), c->name );
		Lux_Cat( line, sizeof( line ), " " );
		Lux_CatInt( line, sizeof( line ), causeScale[c->mod] );
		Lux_Cat( line, sizeof( line ), "%," );
	}
	Lux_Cat( line, sizeof( line ), "\n" );
	trap_Printf( line );
}

static void Lux_CatField( char *line, int size, int w, int off, const luxField_t *fd ) {
	Lux_Cat( line, size, " " );
	Lux_Cat( line, size, fd->name );
	Lux_Cat( line, size, " " );
	if ( fd->type == F_FLOAT ) Lux_CatFloat( line, size, *Lux_Float( w, off ) );
	else Lux_CatInt( line, size, *Lux_Int( w, off ) );
	Lux_Cat( line, size, "," );
}

static void Lux_Dump( void ) {
	char name[64], line[512];
	int w, m, loc, off;
	const char *mn;
	const luxField_t *fd;

	trap_Argv( 1, name, sizeof( name ) );
	if ( Lux_NameEq( name, "player" ) ) {
		Lux_DumpPlayer();
		return;
	}
	w = Lux_FindWeaponLoose( name );
	if ( !w ) {
		trap_Printf( "usage: lux_dump <weapon|player> (unique part of a weapon name), weapons:\n" );
		for ( w = 1; w < WT_COUNT; w++ ) {
			if ( Lux_WeaponName( w ) ) Lux_Print2( "  ", Lux_WeaponName( w ) );
		}
		return;
	}
	line[0] = 0;
	Lux_Cat( line, sizeof( line ), Lux_WeaponName( w ) );
	Lux_Cat( line, sizeof( line ), ":" );
	for ( fd = weaponFields; fd->name; fd++ ) {
		if ( fd->off == WT_RANGE && w != 4 ) continue;	/* only the SPAS uses falloff */
		Lux_CatField( line, sizeof( line ), w, fd->off, fd );
	}
	Lux_Cat( line, sizeof( line ), "\n" );
	trap_Printf( line );
	for ( m = 0; m < WT_NUM_MODES; m++ ) {
		off = WT_MODES + m * MODE_STRIDE;
		mn = *(const char **)( Lux_Weapon( w ) + off );
		if ( !mn ) continue;
		line[0] = 0;
		Lux_Cat( line, sizeof( line ), "  " );
		Lux_Cat( line, sizeof( line ), mn );
		Lux_Cat( line, sizeof( line ), ":" );
		for ( fd = modeFields; fd->name; fd++ ) {
			if ( !Lux_FieldFits( w, off, fd ) ) continue;
			if ( fd->off >= 44 && !*Lux_Int( w, off + 48 ) ) continue;	/* burst fields */
			Lux_CatField( line, sizeof( line ), w, off + fd->off, fd );
		}
		Lux_Cat( line, sizeof( line ), "\n" );
		trap_Printf( line );
	}
	if ( Lux_IsExplosive( w ) ) {
		line[0] = 0;
		Lux_Cat( line, sizeof( line ), "  explosive (-1 = stock):" );
		for ( fd = projFields; fd->name; fd++ ) {
			Lux_Cat( line, sizeof( line ), " " );
			Lux_Cat( line, sizeof( line ), fd->name );
			Lux_Cat( line, sizeof( line ), " " );
			Lux_CatInt( line, sizeof( line ), *(int *)( (char *)&projCfg[w] + fd->off ) );
			Lux_Cat( line, sizeof( line ), "," );
		}
		Lux_Cat( line, sizeof( line ), "\n" );
		trap_Printf( line );
	}
	for ( loc = 1; loc < NUM_HITLOCS; loc++ ) {
		line[0] = 0;
		Lux_Cat( line, sizeof( line ), "  " );
		Lux_Cat( line, sizeof( line ), hitlocNames[loc] );
		Lux_Cat( line, sizeof( line ), ": " );
		Lux_CatFloat( line, sizeof( line ), *Lux_Float( w, WT_HITLOC + loc * 8 ) * 100 );
		Lux_Cat( line, sizeof( line ), *Lux_Int( w, WT_HITLOC + loc * 8 + 4 ) ? " bleed\n" : " nobleed\n" );
		trap_Printf( line );
	}
}

/* list every 4-byte field that differs from stock */
static void Lux_Diff( void ) {
	char line[256];
	int w, off, n = 0;
	int *cur, *old;

	for ( w = 1; w < WT_COUNT; w++ ) {
		for ( off = 0; off < WT_STRIDE; off += 4 ) {
			cur = Lux_Int( w, off );
			old = (int *)( luxBackup + w * WT_STRIDE + off );
			if ( *cur == *old ) continue;
			line[0] = 0;
			Lux_Cat( line, sizeof( line ), "  " );
			Lux_Cat( line, sizeof( line ), Lux_WeaponName( w ) );
			Lux_Cat( line, sizeof( line ), " +" );
			Lux_CatInt( line, sizeof( line ), off );
			Lux_Cat( line, sizeof( line ), ": " );
			Lux_CatInt( line, sizeof( line ), *old );
			Lux_Cat( line, sizeof( line ), " -> " );
			Lux_CatInt( line, sizeof( line ), *cur );
			Lux_Cat( line, sizeof( line ), "\n" );
			trap_Printf( line );
			n++;
		}
	}
	line[0] = 0;
	Lux_Cat( line, sizeof( line ), "luxmod: " );
	Lux_CatInt( line, sizeof( line ), n );
	Lux_Cat( line, sizeof( line ), " fields differ from stock\n" );
	trap_Printf( line );
}

/* ---- explosives: per-projectile overrides and splash knockback -------- */

static unsigned char	inuseBefore[MAX_GENTITIES];

/* HK69 (7), HE (11), smoke (13); flash grenades can't be fired in 4.3 */
static int Lux_IsExplosive( int w ) {
	return w == 7 || w == 11 || w == 13;
}

static void Lux_ResetProjectiles( void ) {
	int w;
	for ( w = 0; w < WT_COUNT; w++ ) {
		projCfg[w].fuse = projCfg[w].radius = projCfg[w].splash = projCfg[w].impact = -1;
		projCfg[w].knock = projCfg[w].self = 100;
	}
}

static unsigned char *Lux_Entity( int i ) {
	return (unsigned char *)G_ENTITIES + i * ENTITY_SIZE;
}

static void Lux_SnapEntities( void ) {
	int i;
	for ( i = 0; i < MAX_GENTITIES; i++ ) inuseBefore[i] = *(int *)( Lux_Entity( i ) + ENT_INUSE ) != 0;
}

/* apply config to projectiles the shooter spawned since Lux_SnapEntities */
static void Lux_FixProjectiles( int shooter, int stockFuse ) {
	unsigned char *e;
	luxProj_t *p;
	int i, w;

	if ( !luxReady ) return;
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		e = Lux_Entity( i );
		if ( inuseBefore[i] || !*(int *)( e + ENT_INUSE ) || *(int *)( e + ENT_PARENT ) != shooter ) continue;
		w = *(int *)( e + ENT_WEAPON );
		if ( w < 1 || w >= WT_COUNT ) continue;
		p = &projCfg[w];
		if ( p->fuse >= 0 ) *(int *)( e + ENT_NEXTTHINK ) += p->fuse - stockFuse;	/* keeps cooking */
		if ( p->radius >= 0 ) *(int *)( e + ENT_SPLASHRAD ) = p->radius;
		if ( p->splash >= 0 ) *(int *)( e + ENT_SPLASHDMG ) = p->splash;
		if ( p->impact >= 0 ) *(int *)( e + ENT_DAMAGE ) = p->impact;
		if ( verbose >= 2 ) {
			char line[160];
			line[0] = 0;
			Lux_Cat( line, sizeof( line ), "luxmod: projectile " );
			Lux_CatInt( line, sizeof( line ), i );
			Lux_Cat( line, sizeof( line ), " weapon " );
			Lux_CatInt( line, sizeof( line ), w );
			Lux_Cat( line, sizeof( line ), " fuse " );
			Lux_CatInt( line, sizeof( line ), *(int *)( e + ENT_NEXTTHINK ) - *(int *)LEVEL_TIME );
			Lux_Cat( line, sizeof( line ), " radius " );
			Lux_CatInt( line, sizeof( line ), *(int *)( e + ENT_SPLASHRAD ) );
			Lux_Cat( line, sizeof( line ), " splash " );
			Lux_CatInt( line, sizeof( line ), *(int *)( e + ENT_SPLASHDMG ) );
			Lux_Cat( line, sizeof( line ), " mod " );
			Lux_CatInt( line, sizeof( line ), *(int *)( e + 708 ) );
			Lux_Cat( line, sizeof( line ), "\n" );
			trap_Printf( line );
		}
	}
}

void hook_fire_hk69( int ent ) {
	Lux_SnapEntities();
	UT_FireHK69( ent );
	Lux_FixProjectiles( ent, 2500 );
}

void hook_fire_grenade( int ent ) {
	Lux_SnapEntities();
	UT_FireGrenade( ent );
	Lux_FixProjectiles( ent, 3500 );
}

void hook_fire_smoke( int ent ) {
	Lux_SnapEntities();
	UT_FireSmoke( ent );
	Lux_FixProjectiles( ent, 15000 );
}

/* every ClientSpawn: stock sets 100 HP, then Start Health applies */
void hook_spawn( int ent ) {
	UT_ClientSpawn( ent );
	if ( luxReady && startHealth != 100 && *(int *)( ent + 520 ) ) {
		*(int *)( ent + ENT_HEALTH ) = startHealth;
		*(int *)( *(int *)( ent + 520 ) + CL_HEALTH ) = startHealth;
	}
}

/* explosive weapon whose splash uses this means of death, or 0 */
static int Lux_ExplosiveForMod( int mod ) {
	int w;
	for ( w = 1; w < WT_COUNT; w++ ) {
		if ( Lux_IsExplosive( w ) && *Lux_Int( w, WT_MOD ) == mod ) return w;
	}
	return 0;
}

/*
 * Every G_Damage call lands here. Untouched unless the means of death belongs
 * to an explosive with Splash Knockback / Self Damage configured; then stock
 * knockback is suppressed and re-applied scaled. That also works in Jump
 * mode, where stock G_Damage returns before knockback for weapon damage.
 */
int hook_damage( int targ, int inflictor, int attacker, float *dir, float *point,
		int damage, int dflags, int mod, int extra ) {
	luxProj_t *p;
	float knock, n[3], len;
	int w;

	if ( !luxReady ) {
		return G_Damage( targ, inflictor, attacker, dir, point, damage, dflags, mod, extra );
	}
	if ( mod >= 0 && mod < 64 && causeScale[mod] != 100 ) {
		damage = damage * causeScale[mod] / 100;
		if ( damage <= 0 ) return 0;
	}

	w = Lux_ExplosiveForMod( mod );
	if ( !w || ( projCfg[w].knock == 100 && projCfg[w].self == 100 ) ) {
		return G_Damage( targ, inflictor, attacker, dir, point, damage, dflags, mod, extra );
	}
	p = &projCfg[w];

	/* stock: splash knockback is damage / 2, capped at 200 */
	knock = damage / 2;
	if ( knock > 200 ) knock = 200;
	if ( dflags & DAMAGE_NO_KNOCKBACK ) knock = 0;
	knock = knock * p->knock / 100;

	if ( targ == attacker ) damage = damage * p->self / 100;
	if ( verbose >= 2 ) {
		char line[160];
		line[0] = 0;
		Lux_Cat( line, sizeof( line ), "luxmod: splash mod " );
		Lux_CatInt( line, sizeof( line ), mod );
		Lux_Cat( line, sizeof( line ), " targ " );
		Lux_CatInt( line, sizeof( line ), ( targ - G_ENTITIES ) / ENTITY_SIZE );
		Lux_Cat( line, sizeof( line ), " attacker " );
		Lux_CatInt( line, sizeof( line ), attacker ? ( attacker - G_ENTITIES ) / ENTITY_SIZE : -1 );
		Lux_Cat( line, sizeof( line ), " damage " );
		Lux_CatInt( line, sizeof( line ), damage );
		Lux_Cat( line, sizeof( line ), " knock " );
		Lux_CatInt( line, sizeof( line ), (int)knock );
		Lux_Cat( line, sizeof( line ), "\n" );
		trap_Printf( line );
	}
	if ( damage > 0 ) {
		G_Damage( targ, inflictor, attacker, dir, point, damage, dflags | DAMAGE_NO_KNOCKBACK, mod, extra );
	}

	if ( knock > 0 && dir && *(int *)( targ + ENT_TAKEDAMAGE ) && *(int *)( targ + 520 ) ) {
		n[0] = dir[0];
		n[1] = dir[1];
		n[2] = dir[2];
		len = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
		if ( len > 0 ) {
			len = sqrt( len );
			n[0] /= len; n[1] /= len; n[2] /= len;
			G_Knockback( targ, n, knock );
		}
	}
	return 0;
}

static int Lux_ClientNum( unsigned char *cl );
static int Lux_IsAlive( unsigned char *cl );

/* lux_throw <player> <he|smoke|hk69>: make a player fire a projectile where they look */
static void Lux_Throw( void ) {
	char target[64], what[32];
	unsigned char *cl, *ent;
	int w, old;

	trap_Argv( 1, target, sizeof( target ) );
	trap_Argv( 2, what, sizeof( what ) );
	w = Lux_NameEq( what, "he" ) ? 11 : Lux_NameEq( what, "smoke" ) ? 13 :
		Lux_NameEq( what, "hk69" ) ? 7 : 0;
	if ( !w || trap_Argc() < 3 ) {
		trap_Printf( "usage: lux_throw <player> <he|smoke|hk69>\n" );
		return;
	}
	cl = UT_ClientFromString( target );
	if ( !cl ) return;
	if ( !Lux_IsAlive( cl ) ) { Lux_Print2( "player is not alive", "" ); return; }
	ent = Lux_Entity( Lux_ClientNum( cl ) );
	old = *(int *)( ent + ENT_WEAPON );
	*(int *)( ent + ENT_WEAPON ) = w;
	UT_FireWeapon( (int)ent );
	*(int *)( ent + ENT_WEAPON ) = old;
}

/* ---- player commands: gh, gw, gi -------------------------------------- */

static const char *itemNames[NUM_ITEMS] = {
	"Kevlar Vest", "TacGoggles", "Medkit", "Silencer", "Laser Sight", "Helmet", "Extra Ammo"
};

static unsigned char *Lux_ClientPtr( int i ) {
	return *(unsigned char **)LEVEL_CLIENTS + i * CLIENT_SIZE;
}

static int Lux_ClientNum( unsigned char *cl ) {
	return ( cl - *(unsigned char **)LEVEL_CLIENTS ) / CLIENT_SIZE;
}

static int Lux_ClientInt( unsigned char *cl, int off ) {
	return *(int *)( cl + off );
}

static int Lux_IsAlive( unsigned char *cl ) {
	return Lux_ClientInt( cl, CL_CONNECTED ) == 2 && Lux_ClientInt( cl, CL_TEAM ) != 3
		&& !Lux_ClientInt( cl, CL_GHOST ) && Lux_ClientInt( cl, CL_HEALTH ) > 0;
}

static const char *Lux_ClientName( unsigned char *cl ) {
	static char buf[16];
	buf[0] = 0;
	Lux_Cat( buf, sizeof( buf ), "client " );
	Lux_CatInt( buf, sizeof( buf ), Lux_ClientNum( cl ) );
	return buf;
}

static void Lux_Report( unsigned char *cl, const char *what ) {
	Lux_Print2( Lux_ClientName( cl ), what );
}

static void Lux_SetHealth( unsigned char *cl, const char *arg ) {
	unsigned char *ent = (unsigned char *)G_ENTITIES + Lux_ClientNum( cl ) * ENTITY_SIZE;
	float f;
	int hp;

	Lux_ParseNum( arg, &f );
	hp = (int)f;
	while ( *arg == ' ' ) arg++;
	if ( *arg == '+' || *arg == '-' ) hp += Lux_ClientInt( cl, CL_HEALTH );
	if ( hp < 1 ) hp = 1;
	if ( hp > 100 ) hp = 100;
	*(int *)( ent + ENT_HEALTH ) = hp;
	*(int *)( cl + CL_HEALTH ) = hp;
}

/* full magazine and spare magazines, whether the weapon is new or not */
static int Lux_GiveWeapon( unsigned char *cl, int w ) {
	int slot, mode, *inv;

	slot = UT_FindWeaponSlot( cl, w );
	if ( slot < 0 ) {
		mode = cl[CL_WEAPMODES + w] - '0';
		if ( mode < 0 || mode >= WT_NUM_MODES ) mode = 0;
		return UT_GiveWeapon( cl, w, mode ) >= 0;
	}
	inv = (int *)( cl + CL_INVENTORY ) + slot;
	*inv = ( *inv & 0x00ff00ff ) | ( ( *Lux_Int( w, WT_AMMO ) & 255 ) << 8 )
		| ( ( *Lux_Int( w, WT_CLIPS ) & 255 ) << 24 );
	return 1;
}

/* weapon/item spec: a name ("lr300", "medkit") or letters ("hk", "acg") */
static int Lux_ParseSpec( const char *spec, int items, int *out, int max ) {
	int n = 0, i, id;
	const char *c;

	if ( items ) {
		for ( i = 0; i < NUM_ITEMS; i++ ) {
			if ( Lux_NameEq( itemNames[i], spec ) || ( spec[0] && spec[1] && Lux_NameEq( itemNames[i] + ( i == 0 ? 7 : 0 ), spec ) ) ) {
				out[0] = FIRST_ITEM + i;
				return 1;
			}
		}
	} else if ( ( id = Lux_FindWeaponLoose( spec ) ) != 0 ) {
		out[0] = id;
		return 1;
	}
	for ( c = spec; *c && n < max; c++ ) {
		id = Lux_Lower( *c ) - 'a';
		if ( items ) {
			if ( id < 0 || id >= NUM_ITEMS ) return -1;
			out[n++] = FIRST_ITEM + id;
		} else {
			id++;	/* a = 1 = Knife ... s = 19 = M4A1 ... z = 26 = Magnum */
			if ( id < 1 || id >= WT_COUNT || !Lux_WeaponName( id ) || id == 16 ) return -1;
			out[n++] = id;
		}
	}
	return n;
}

static void Lux_PlayerCommand( int which ) {
	char target[64], arg[64], line[256];
	int ids[32], n, i, k, ok;
	unsigned char *cl;

	if ( trap_Argc() < 3 ) {
		if ( which == 'h' ) trap_Printf( "usage: gh <player|all> <health>   (+N/-N adds)\n" );
		if ( which == 'w' ) trap_Printf( "usage: gw <player|all> <weapon name or letters>\n" );
		if ( which == 'i' ) trap_Printf( "usage: gi <player|all> <item name or letters a-g>\n" );
		return;
	}
	trap_Argv( 1, target, sizeof( target ) );
	trap_Argv( 2, arg, sizeof( arg ) );

	n = 0;
	if ( which != 'h' ) {
		n = Lux_ParseSpec( arg, which == 'i', ids, 32 );
		if ( n <= 0 ) { Lux_Print2( "unknown weapon/item: ", arg ); return; }
	}

	for ( i = 0; i < *(int *)LEVEL_MAXCLIENTS; i++ ) {
		if ( Lux_NameEq( target, "all" ) ) {
			cl = Lux_ClientPtr( i );
			if ( !Lux_IsAlive( cl ) ) continue;
		} else {
			if ( i ) break;
			cl = UT_ClientFromString( target );
			if ( !cl ) return;
			if ( !Lux_IsAlive( cl ) ) { Lux_Report( cl, " is not alive" ); return; }
		}
		line[0] = 0;
		if ( which == 'h' ) {
			Lux_SetHealth( cl, arg );
			Lux_Cat( line, sizeof( line ), ": health " );
			Lux_CatInt( line, sizeof( line ), Lux_ClientInt( cl, CL_HEALTH ) );
		}
		for ( k = 0; k < n; k++ ) {
			ok = which == 'w' ? Lux_GiveWeapon( cl, ids[k] ) : UT_GiveItem( cl, ids[k] ) >= 0;
			Lux_Cat( line, sizeof( line ), ok ? " +" : " (no slot) " );
			Lux_Cat( line, sizeof( line ), which == 'w' ? Lux_WeaponName( ids[k] ) : itemNames[ids[k] - FIRST_ITEM] );
		}
		Lux_Report( cl, line );
	}
}

/* ---- hooks (vmMain's calls are redirected here) ----------------------- */

void hook_init( int levelTime, int randomSeed, int restart ) {
	lux_blob_init();
	G_InitGame( levelTime, randomSeed, restart );
	Lux_Load();
}

int hook_console( void ) {
	char cmd[64];

	trap_Argv( 0, cmd, sizeof( cmd ) );
	if ( Lux_NameEq( cmd, "lux_reload" ) ) {
		Lux_Load();
		return 1;
	}
	if ( Lux_NameEq( cmd, "lux_dump" ) ) {
		Lux_Dump();
		return 1;
	}
	if ( Lux_NameEq( cmd, "lux_diff" ) ) {
		Lux_Diff();
		return 1;
	}
	if ( Lux_NameEq( cmd, "lux_throw" ) ) {
		Lux_Throw();
		return 1;
	}
	if ( Lux_NameEq( cmd, "gh" ) || Lux_NameEq( cmd, "gw" ) || Lux_NameEq( cmd, "gi" ) ) {
		Lux_PlayerCommand( Lux_Lower( cmd[1] ) );
		return 1;
	}
	return ConsoleCommand();
}
