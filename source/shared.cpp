#include "shared.hpp"

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/Lua/Helpers.hpp>
#include <GarrysMod/Lua/LuaInterface.h>
#include <GarrysMod/Lua/LuaGameCallback.h>
#include <GarrysMod/Lua/AutoReference.h>
#include <GarrysMod/InterfacePointers.hpp>
#include <lua.hpp>

#include <detouring/hook.hpp>

#include <string>
#include <sstream>

#include <filesystem_stdio.h>

namespace shared
{

static bool runtime = false;
static std::string runtime_error;
static GarrysMod::Lua::AutoReference runtime_stack;
static CFileSystem_Stdio *filesystem = nullptr;
static bool runtime_detoured = false;
static bool compiletime_detoured = false;
static GarrysMod::Lua::CFunc AdvancedLuaErrorReporter = nullptr;
static Detouring::Hook AdvancedLuaErrorReporter_detour;

struct ErrorProperties
{
	std::string source_file;
	int32_t source_line = -1;
	std::string error_string;
};

static int32_t PushErrorProperties( GarrysMod::Lua::ILuaInterface *lua, std::istringstream &error, ErrorProperties &props )
{
	std::string source_file;
	std::getline( error, source_file, ':' );

	int32_t source_line = -1;
	error >> source_line;

	error.ignore( 2 ); // ignore ": "

	std::string error_string;
	std::getline( error, error_string );

	if( !error ) // our stream is still valid
	{
		lua->PushNil( );
		lua->PushNil( );
		lua->PushNil( );
		return 3;
	}

	props.source_file = source_file;
	props.source_line = source_line;
	props.error_string = error_string;

	lua->PushString( props.source_file.c_str( ) );
	lua->PushNumber( props.source_line );
	lua->PushString( props.error_string.c_str( ) );
	return 3;
}

int32_t PushErrorProperties( GarrysMod::Lua::ILuaInterface *lua, std::istringstream &error )
{
	ErrorProperties props;
	return PushErrorProperties( lua, error, props );
}

inline bool GetUpvalues( GarrysMod::Lua::ILuaInterface *lua, int32_t funcidx )
{
	if( funcidx < 0 )
		funcidx = lua->Top( ) + funcidx + 1;

	int32_t idx = 1;
	const char *name = lua->GetUpvalue( funcidx, idx );
	if( name == nullptr )
		return false;

	// Keep popping until we either reach the end or until we reach a valid upvalue
	while( name[0] == '\0' )
	{
		lua->Pop( 1 );

		if( ( name = lua->GetUpvalue( funcidx, ++idx ) ) == nullptr )
			return false;
	}

	lua->CreateTable( );

	// Push the last upvalue to the top
	lua->Push( -2 );
	// And remove it from its previous location
	lua->Remove( -3 );

	do
		if( name[0] != '\0' )
			lua->SetField( -2, name );
		else
			lua->Pop( 1 );
	while( ( name = lua->GetUpvalue( funcidx, ++idx ) ) != nullptr );

	return true;
}

inline bool GetLocals( GarrysMod::Lua::ILuaInterface *lua, lua_Debug &dbg )
{
	int32_t idx = 1;
	const char *name = lua->GetLocal( &dbg, idx );
	if( name == nullptr )
		return false;

	// Keep popping until we either reach the end or until we reach a valid local
	while( name[0] == '(' )
	{
		lua->Pop( 1 );

		if( ( name = lua->GetLocal( &dbg, ++idx ) ) == nullptr )
			return false;
	}

	lua->CreateTable( );

	// Push the last local to the top
	lua->Push( -2 );
	// And remove it from its previous location
	lua->Remove( -3 );

	do
		if( name[0] != '(' )
			lua->SetField( -2, name );
		else
			lua->Pop( 1 );
	while( ( name = lua->GetLocal( &dbg, ++idx ) ) != nullptr );

	return true;
}

static int32_t PushStackTable( GarrysMod::Lua::ILuaInterface *lua )
{
	lua->CreateTable( );

	int32_t lvl = 0;
	lua_Debug dbg;
	while( lua->GetStack( lvl, &dbg ) == 1 && lua->GetInfo( "SfLlnu", &dbg ) == 1 )
	{
		lua->PushNumber( ++lvl );
		lua->CreateTable( );

		if( GetUpvalues( lua, -4 ) )
			lua->SetField( -2, "upvalues" );

		if( GetLocals( lua, dbg ) )
			lua->SetField( -2, "locals" );

		lua->Push( -4 );
		lua->SetField( -2, "func" );

		lua->Push( -3 );
		lua->SetField( -2, "activelines" );

		lua->PushNumber( dbg.event );
		lua->SetField( -2, "event" );

		lua->PushString( dbg.name != nullptr ? dbg.name : "" );
		lua->SetField( -2, "name" );

		lua->PushString( dbg.namewhat != nullptr ? dbg.namewhat : "" );
		lua->SetField( -2, "namewhat" );

		lua->PushString( dbg.what != nullptr ? dbg.what : "" );
		lua->SetField( -2, "what" );

		lua->PushString( dbg.source != nullptr ? dbg.source : "" );
		lua->SetField( -2, "source" );

		lua->PushNumber( dbg.currentline );
		lua->SetField( -2, "currentline" );

		lua->PushNumber( dbg.nups );
		lua->SetField( -2, "nups" );

		lua->PushNumber( dbg.linedefined );
		lua->SetField( -2, "linedefined" );

		lua->PushNumber( dbg.lastlinedefined );
		lua->SetField( -2, "lastlinedefined" );

		lua->PushString( dbg.short_src );
		lua->SetField( -2, "short_src" );

		lua->SetTable( -5 );

		// Pop activelines and func
		lua->Pop( 2 );
	}

	return 1;
}

inline const IAddonSystem::Information *FindWorkshopAddonFromFile( const std::string &source )
{
	if( source.empty( ) || source == "[C]" )
		return nullptr;

	const auto addons = filesystem->Addons( );
	if( addons == nullptr )
		return nullptr;

	return addons->FindFileOwner( source );
}

static int32_t AdvancedLuaErrorReporter_d( lua_State *state )
{
	auto LUA = static_cast<GarrysMod::Lua::ILuaInterface *>( state->luabase );

	const char *errstr = LUA->GetString( 1 );

	runtime = true;

	if( errstr != nullptr )
		runtime_error = errstr;
	else
		runtime_error.clear( );

	PushStackTable( LUA );
	runtime_stack.Create( );

	return AdvancedLuaErrorReporter_detour.GetTrampoline<GarrysMod::Lua::CFunc>( )( state );
}

class CLuaGameCallback : public GarrysMod::Lua::ILuaGameCallback
{
public:
	CLuaGameCallback( ) :
		lua( nullptr ),
		callback( nullptr )
	{ }

	~CLuaGameCallback( )
	{
		Reset( );
	}

	GarrysMod::Lua::ILuaObject *CreateLuaObject( )
	{
		return callback->CreateLuaObject( );
	}

	void DestroyLuaObject( GarrysMod::Lua::ILuaObject *obj )
	{
		callback->DestroyLuaObject( obj );
	}

	void ErrorPrint( const char *error, bool print )
	{
		callback->ErrorPrint( error, print );
	}

	void Msg( const char *msg, bool useless )
	{
		callback->Msg( msg, useless );
	}

	void MsgColour( const char *msg, const Color &color )
	{
		callback->MsgColour( msg, color );
	}

	void LuaError( const CLuaError *error )
	{
		const int32_t funcs = LuaHelpers::PushHookRun( lua, "LuaError" );
		if( funcs == 0 )
			return callback->LuaError( error );

		const std::string &errstr = runtime ? runtime_error : error->message;

		lua->PushBool( runtime );
		lua->PushString( errstr.c_str( ) );

		std::istringstream errstream( errstr );
		ErrorProperties props;
		int32_t args = PushErrorProperties( lua, errstream, props );

		if( runtime )
		{
			args += 1;
			runtime_stack.Push( );
			runtime_stack.Free( );
		}
		else
			args += PushStackTable( lua );

		runtime = false;

		const auto source_addon = FindWorkshopAddonFromFile( props.source_file );
		if( source_addon == nullptr )
		{
			lua->PushNil( );
			lua->PushNil( );
		}
		else
		{
			lua->PushString( source_addon->title.c_str( ) );
			lua->PushString( std::to_string( source_addon->wsid ).c_str( ) );
		}

		if( !LuaHelpers::CallHookRun( lua, 4 + args, 1 ) )
			return callback->LuaError( error );

		const bool proceed = !lua->IsType( -1, GarrysMod::Lua::Type::BOOL ) || !lua->GetBool( -1 );
		lua->Pop( 1 );
		if( proceed )
			return callback->LuaError( error );
	}

	void InterfaceCreated( GarrysMod::Lua::ILuaInterface *iface )
	{
		callback->InterfaceCreated( iface );
	}

	void SetLua( GarrysMod::Lua::ILuaInterface *iface )
	{
		lua = static_cast<GarrysMod::Lua::CLuaInterface *>( iface );
		callback = lua->GetLuaGameCallback( );
	}

	void Detour( )
	{
		lua->SetLuaGameCallback( this );
	}

	void Reset( )
	{
		lua->SetLuaGameCallback( callback );
	}

private:
	GarrysMod::Lua::CLuaInterface *lua;
	GarrysMod::Lua::ILuaGameCallback *callback;
};

static CLuaGameCallback callback;

inline void DetourCompiletime( )
{
	if( compiletime_detoured )
		return;

	if( !runtime_detoured )
		callback.Detour( );

	compiletime_detoured = true;
}

inline void ResetCompiletime( )
{
	if( !compiletime_detoured )
		return;

	if( !runtime_detoured )
		callback.Reset( );

	compiletime_detoured = false;
}

inline void DetourRuntime( )
{
	if( runtime_detoured )
		return;

	if( !compiletime_detoured )
		callback.Detour( );

	AdvancedLuaErrorReporter_detour.Enable( );
	runtime_detoured = true;
}

inline void ResetRuntime( )
{
	if( !runtime_detoured )
		return;

	if( !compiletime_detoured )
		callback.Reset( );

	AdvancedLuaErrorReporter_detour.Disable( );
	runtime_detoured = false;
}

LUA_FUNCTION_STATIC( EnableRuntimeDetour )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );

	if( LUA->GetBool( 1 ) )
		DetourRuntime( );
	else
		ResetRuntime( );

	LUA->PushBool( true );
	return 1;
}

LUA_FUNCTION_STATIC( EnableCompiletimeDetour )
{
	LUA->CheckType( 1, GarrysMod::Lua::Type::BOOL );

	if( LUA->GetBool( 1 ) )
		DetourCompiletime( );
	else
		ResetCompiletime( );

	LUA->PushBool( true );
	return 1;
}

LUA_FUNCTION_STATIC( FindWorkshopAddonFileOwnerLua )
{
	const char *path = LUA->CheckString( 1 );

	const auto owner = FindWorkshopAddonFromFile( path );
	if( owner == nullptr )
		return 0;

	LUA->PushString( owner->title.c_str( ) );
	LUA->PushString( std::to_string( owner->wsid ).c_str( ) );
	return 2;
}

void Initialize( GarrysMod::Lua::ILuaBase *LUA )
{
	runtime_stack.Setup( LUA );

	callback.SetLua( static_cast<GarrysMod::Lua::ILuaInterface *>( LUA ) );

	LUA->ReferencePush( 1 );
	if( !LUA->IsType( -1, GarrysMod::Lua::Type::FUNCTION ) )
		LUA->ThrowError( "reference to AdvancedLuaErrorReporter is invalid" );

	AdvancedLuaErrorReporter = LUA->GetCFunction( -1 );
	if( AdvancedLuaErrorReporter == nullptr )
		LUA->ThrowError( "unable to obtain AdvancedLuaErrorReporter" );

	LUA->Pop( 1 );

	if( !AdvancedLuaErrorReporter_detour.Create(
		AdvancedLuaErrorReporter, reinterpret_cast<void *>( &AdvancedLuaErrorReporter_d )
	) )
		LUA->ThrowError( "unable to create a hook for AdvancedLuaErrorReporter" );

	filesystem = static_cast<CFileSystem_Stdio *>( InterfacePointers::FileSystem( ) );
	if( filesystem == nullptr )
		LUA->ThrowError( "unable to initialize IFileSystem" );

	LUA->PushCFunction( EnableRuntimeDetour );
	LUA->SetField( -2, "EnableRuntimeDetour" );

	LUA->PushCFunction( EnableCompiletimeDetour );
	LUA->SetField( -2, "EnableCompiletimeDetour" );

	LUA->PushCFunction( FindWorkshopAddonFileOwnerLua );
	LUA->SetField( -2, "FindWorkshopAddonFileOwner" );
}

void Deinitialize( GarrysMod::Lua::ILuaBase * )
{
	ResetRuntime( );
	ResetCompiletime( );
	AdvancedLuaErrorReporter_detour.Destroy( );
}

}
