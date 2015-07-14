gm_luaerror
===========

A module for Garry's Mod that adds hooks for obtaining errors that happen on the client and server (if activated on server, it also pushes errors from clients).

Info
-------

	luaerror.Version -- holds the luaerror module version in a string form
	luaerror.VersionNum -- holds the luaerror module version in a numeric form, LuaJIT style

	luaerror.EnableRuntimeDetour(boolean) -- enable/disable Lua runtime errors
	luaerror.EnableCompiletimeDetour(boolean) -- enable/disable Lua compiletime errors

	luaerror.EnableClientDetour(boolean) -- enable/disable Lua errors from clients (serverside only)
	-- returns nil followed by an error string in case of failure to detour

	Hooks:
	LuaError(isruntime, sourcefile, sourceline, errorstr, stack)
	-- isruntime is a boolean saying whether this is a runtime error or not
	-- sourcefile is a string which is the source file of the error
	-- sourceline is a number which is the source line of the error
	-- errorstr is a string which is the error itself
	-- stack is a table containing the Lua stack at the time of the error

	ClientLuaError(player, errorstr)
	-- player is a Player object which indicates who errored
	-- errorstr is a string which is the error itself

Mac was not tested at all (sorry but I'm poor).

If stuff starts erroring or fails to work, be sure to check the correct line endings (\n and such) are present in the files for each OS.

This project requires garrysmod_common (https://bitbucket.org/danielga/garrysmod_common), a framework to facilitate the creation of compilations files (Visual Studio, make, XCode, etc). Simply set the environment variable 'GARRYSMOD_COMMON' or the premake option 'gmcommon' to the path of your local copy of garrysmod_common. We also use the SourceSDK, so set the environment variable 'SOURCE_SDK' or the premake option 'sourcesdk' to the path of your local copy of the SourceSDK (SourceSDK2013 is recommended, https://github.com/ValveSoftware/source-sdk-2013).