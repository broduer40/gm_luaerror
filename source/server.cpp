#include <server.hpp>
#include <shared.hpp>
#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/Lua/LuaInterface.h>
#include <lua.hpp>
#include <cstdint>
#include <GarrysMod/Interfaces.hpp>
#include <symbolfinder.hpp>
#include <detours.h>
#include <memory>
#include <sstream>
#include <algorithm>
#include <functional>
#include <cctype>
#include <eiface.h>
#include <../game/server/player.h>

IVEngineServer *engine = nullptr;

namespace server
{

#if defined _WIN32

static const char HandleClientLuaError_sym[] = "\x55\x8B\xEC\x83\xEC\x08\xA1\x2A\x2A\x2A\x2A\xF3\x0F\x10\x00\x56";
static const size_t HandleClientLuaError_symlen = sizeof( HandleClientLuaError_sym ) - 1;

#elif defined __linux

#if IS_SERVERSIDE

static const char HandleClientLuaError_sym[] = "@_Z20HandleClientLuaErrorP11CBasePlayerPKc";
static const size_t HandleClientLuaError_symlen = 0;

#else

static const char HandleClientLuaError_sym[] = "\x55\x89\xE5\x57\x56\x53\x83\xEC\x4C\x65\xA1\x2A\x2A\x2A\x2A\x89\x45\xE4";
static const size_t HandleClientLuaError_symlen = sizeof( HandleClientLuaError_sym ) - 1;

#endif

#elif defined __APPLE__

static const char HandleClientLuaError_sym[] = "@__Z20HandleClientLuaErrorP11CBasePlayerPKc";
static const size_t HandleClientLuaError_symlen = 0;

#endif

static const std::string main_binary = Helpers::GetBinaryFileName(
	"server",
	false,
	true,
	"garrysmod/bin/"
);
static SourceSDK::FactoryLoader engine_loader( "engine", false );
static GarrysMod::Lua::ILuaInterface *lua = nullptr;

typedef void( *HandleClientLuaError_t )( CBasePlayer *player, const char *error );

static std::unique_ptr< MologieDetours::Detour<HandleClientLuaError_t> > HandleClientLuaError_detour;
static HandleClientLuaError_t HandleClientLuaError = nullptr;

inline std::string Trim( const std::string &s )
{
	std::string c = s;
	c.erase(
		std::find_if(
			c.rbegin( ),
			c.rend( ),
			std::not1( std::ptr_fun<int, int>( std::isspace ) )
		).base( ),
		c.end( )
	); // remote trailing "spaces"
	c.erase(
		c.begin( ),
		std::find_if(
			c.begin( ),
			c.end( ),
			std::not1( std::ptr_fun<int, int>( std::isspace ) )
		)
	); // remote initial "spaces"
	return c;
}

static void HandleClientLuaError_d( CBasePlayer *player, const char *error )
{
	int32_t funcs = shared::PushHookRun( lua, "ClientLuaError" );
	if( funcs == 0 )
		return HandleClientLuaError_detour->GetOriginalFunction( )( player, error );

	int32_t args = 2;
	lua->PushString( "ClientLuaError" );

	lua->GetField( GarrysMod::Lua::INDEX_GLOBAL, "Entity" );
	if( !lua->IsType( -1, GarrysMod::Lua::Type::FUNCTION ) )
	{
		lua->Pop( funcs + args );
		lua->ErrorNoHalt( "[ClientLuaError] Global Entity is not a function!\n" );
		return HandleClientLuaError_detour->GetOriginalFunction( )( player, error );
	}
	lua->PushNumber( player->entindex( ) );
	lua->Call( 1, 1 );

	std::string cleanerror = Trim( error );
	if( cleanerror.compare( 0, 8, "[ERROR] " ) == 0 )
		cleanerror = cleanerror.erase( 0, 8 );

	args += 2;
	lua->PushString( cleanerror.c_str( ) );

	std::istringstream errstream( cleanerror );
	args += shared::PushErrorProperties( lua, errstream );

	lua->CreateTable( );
	while( errstream )
	{
		int32_t level = 0;
		errstream >> level;

		errstream.ignore( 2 ); // ignore ". "

		std::string name;
		errstream >> name;

		errstream.ignore( 3 ); // ignore " - "

		std::string source;
		std::getline( errstream, source, ':' );

		int32_t currentline = -1;
		errstream >> currentline;

		if( !errstream ) // it shouldn't have reached eof by now
			break;

		lua->PushNumber( level );
		lua->CreateTable( );

		lua->PushString( name.c_str( ) );
		lua->SetField( -2, "name" );

		lua->PushNumber( currentline );
		lua->SetField( -2, "currentline" );

		lua->PushString( source.c_str( ) );
		lua->SetField( -2, "source" );

		lua->SetTable( -3 );
	}

	if( shared::RunHook( lua, "ClientLuaError", args, funcs ) )
		return HandleClientLuaError_detour->GetOriginalFunction( )( player, error );
}

LUA_FUNCTION_STATIC( EnableClientDetour )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );

	bool enable = LUA->GetBool( 1 );
	if( enable && !HandleClientLuaError_detour )
	{
		bool errored = false;
		try
		{
			HandleClientLuaError_detour.reset( new MologieDetours::Detour<HandleClientLuaError_t>(
				HandleClientLuaError, HandleClientLuaError_d
			) );
		}
		catch( const std::exception &e )
		{
			errored = true;
			LUA->PushNil( );
			LUA->PushString( e.what( ) );
		}

		if( errored )
			return 2;
	}
	else if( !enable && HandleClientLuaError_detour )
		HandleClientLuaError_detour.reset( );

	LUA->PushBool( true );
	return 1;
}

void Initialize( GarrysMod::Lua::ILuaBase *LUA )
{
	lua = static_cast<GarrysMod::Lua::ILuaInterface *>( LUA );

	engine = engine_loader.GetInterface<IVEngineServer>( INTERFACEVERSION_VENGINESERVER );
	if( engine == nullptr )
		LUA->ThrowError( "failed to retrieve server engine interface" );

	SymbolFinder symfinder;

	HandleClientLuaError = reinterpret_cast<HandleClientLuaError_t>( symfinder.ResolveOnBinary(
			main_binary.c_str( ), HandleClientLuaError_sym, HandleClientLuaError_symlen
	) );
	if( HandleClientLuaError == nullptr )
		LUA->ThrowError( "unable to sigscan function HandleClientLuaError" );

	LUA->PushCFunction( EnableClientDetour );
	LUA->SetField( -2, "EnableClientDetour" );
}

void Deinitialize( GarrysMod::Lua::ILuaBase * )
{
	if( HandleClientLuaError_detour )
		HandleClientLuaError_detour.reset( );
}

}
