// Copyright(C) 1993-2024 Id Software, Inc.
// Copyright(C) 2024 Ethan Watson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
//
// ID24 data tables and supporting code 0.99.1
//
// This file represents an updated thinker object with some type safety.
// It achieves this by storing the thinker function as the first value
// in the object and combines compile time checks with comparisons of
// the thinker function to pre-defined allowed functions.
//
// This is C++17 compatible. It's easy enough to strip std::enable_if
// and replace with requires if you want to upgrade it to C++20.
//
// There's two bits of functionality that are critical to making this
// all work:
//  * actionf_t performing invokation type safety
//  * thinker_cast< T > performing object casting type safety
//
// We need thinker_cast to be defined first. For each thinker type you
// want to be included in this system, you must declare it by invoking
// the MakeThinkFuncLookup macro. This takes a type and every function
// that can be set on that object as parameters. For example:
//
//    MakeThinkFuncLookup( mobj_t, P_MobjThinker );
//
// That macro will create the thinkfunclookup< mobj_t > specialisation
// and store P_MobjThinker in a constexpr std::array. It will also
// create specialisations for thinker_cast< mobj_t > to allow any
// pointer to be cast to a mobj_t if it is safe to do so.
//
// The thinker_cast works by treating the object to be cast as a void**
// and dereferencing that to find a void* that it assumes to be a function
// pointer. To facilitate that, it is expected that the first value in
// an object is a thinker_t. thinker_t has been rewritten to place the
// function pointer as the first value, and to allow enabling and disabling
// the thinker via a flag rather than de facto disabling it with a null
// pointer. This ensures that the type safety achieved by thinker_cast
// keeps running, and will require modifying your code to handle that
// with platforms and ceilings in particular.
//
// actionf_t is no longer just a typedef, but an object that handles
// invokation safety with a combination of compile-time parameter checks
// and run-time checks of the type of function pointer set. It uses
// operator() overloads, and as such can work as a drop-in replacement
// for the prior actionf_t declaration. As type- and invokation- safety
// is a requirement for ID24, actionf_t will I_Error if it is invoked
// with incorrect parameters. You may want to catch this with the
// CanExecuteWith method to check if your parameters are valid prior
// to invokation and take a different error-handling path.
//
// As not all thinkers are mobjs, the invocation for the single-parameter
// thinker function accepts any type that can thinker_cast.

#include <type_traits>

typedef struct mobj_s mobj_t;
typedef struct player_s player_t;
typedef struct pspdef_s pspdef_t;
typedef struct thinker_s thinker_t;
typedef void (*actionf_v)();
typedef void (*actionf_p1)( thinker_t* );
typedef void (*actionf_p2)( player_t*, pspdef_t* );

template< typename _ty >
struct thinkfunclookup
{
protected:
	template< typename... _args >
	static constexpr std::array< void*, sizeof...( _args ) > toarray( _args&&... args)
	{
		return { (void*)args... };
	}
};

#define MakeThinkFuncLookup( type, ... ) \
template<> \
constexpr thinker_t* thinker_cast< thinker_t >( type* from ) \
{ \
	return (thinker_t*)from; \
} \
 \
template<> \
struct thinkfunclookup< type > : thinkfunclookup< void > \
{ \
	constexpr static auto funcs = toarray( __VA_ARGS__ ); \
}

template< typename _to, typename _from >
constexpr _to* thinker_cast( _from* from )
{
	auto found = std::find( thinkfunclookup< _to >::funcs.begin(), thinkfunclookup< _to >::funcs.end(), *(void**)from );
	return found == thinkfunclookup< _to >::funcs.end() ? nullptr : (_to*)from;
}

template<>
constexpr thinker_t* thinker_cast< thinker_t >( thinker_t* val )
{
	return val;
}

typedef enum actiontype_e
{
	at_none,
	at_void,
	at_p1,
	at_p2,

	at_disabled = 0x80000000,
} actiontype_t;

using underlyingactiontype_t = std::underlying_type_t< actiontype_t >;

constexpr actiontype_t operator|( const actiontype_t lhs, const actiontype_t rhs )
{
	return (actiontype_t)( (underlyingactiontype_t)lhs | (underlyingactiontype_t)rhs );
};

constexpr actiontype_t operator&( const actiontype_t lhs, const actiontype_t rhs )
{
	return (actiontype_t)( (underlyingactiontype_t)lhs & (underlyingactiontype_t)rhs );
};

constexpr actiontype_t operator^( const actiontype_t lhs, const actiontype_t rhs )
{
	return (actiontype_t)( (underlyingactiontype_t)lhs ^ (underlyingactiontype_t)rhs );
};

typedef struct actionf_s
{
	actionf_s()
		: _acv( nullptr )
		, type( at_none )
	{
	}

	actionf_s( actionf_v func )
		: _acv( func )
		, type( at_void )
	{
	}

	actionf_s( actionf_p1 func )
		: _acp1( func )
		, type( at_p1 )
	{
	}

	actionf_s( actionf_p2 func )
		: _acp2( func )
		, type( at_p2 )
	{
	}

	actionf_s(std::nullptr_t v)
		: _acv( v )
		, type( at_none )
	{
	}

	template< typename _param,
			std::enable_if_t< std::is_same_v< _param*, decltype( thinker_cast< _param >( (_param*)nullptr ) ) >, bool > = true >
	actionf_s( void(*func)(_param*) )
		: _acp1( (actionf_p1)func )
		, type( at_p1 )
	{
	}

	actionf_s& operator=(std::nullptr_t v)
	{
		_acv = v;
		type = at_none;
		return *this;
	}

	template< typename _param >
	actionf_s& operator=( void(*func)(_param*) )
	{
		if constexpr( !std::is_same_v< _param*, decltype( thinker_cast< _param >( (_param*)nullptr ) ) > )
		{
			I_Error( "Non-thinker assignment of actionf_t" );
		}

		_acp1 = (actionf_p1)func;
		type = at_p1;
		return *this;
	}

	void operator()() const
	{
		if( Type() != at_void )
		{
			I_Error( "Invoking wrong actionf_t type" );
		}
		_acv();
	}

	template< typename _param >
	void operator()( _param* val ) const
	{
		if( Type() != at_p1 )
		{
			I_Error( "Invoking wrong actionf_t type" );
		}

		if constexpr( !std::is_same_v< _param*, decltype( thinker_cast< _param >( (_param*)nullptr ) ) > )
		{
			I_Error( "Non-thinker invocation of actionf_t" );
		}

		_acp1( (thinker_t*)val );
	}

	void operator()( player_t* player, pspdef_t* pspr ) const
	{
		if( Type() != at_p2 )
		{
			I_Error( "Invoking wrong actionf_t type" );
		}

		_acp2( player, pspr );
	}

	template< typename _param >
	bool operator==( void(*func)(_param*) ) const
	{
		if constexpr( !std::is_same_v< _param*, decltype( thinker_cast< _param >( (_param*)nullptr ) ) > )
		{
			I_Error( "Non-thinker comparison of actionf_t" );
		}

		return Type() == at_p1 && _acp1 == (actionf_p1)func;
	}

	template< typename _param1, typename _param2 >
	bool operator==(void(*func)(_param1*, _param2*)) const
	{
		return Type() == at_p2 && _acp2 == (actionf_p2)func;
	}

	constexpr const bool Valid() const
	{
		return Type() != at_none && _acv != nullptr;
	}

	constexpr bool Enabled() const
	{
		return (type & at_disabled) != at_disabled;
	}

	constexpr void Disable()
	{
		type = (type | at_disabled);
	}

	constexpr void Enable()
	{
		type = (actiontype_t)(type & ~at_disabled);
	}

	constexpr actionf_v Value()					{ return _acv; }
	constexpr const actionf_v Value() const		{ return _acv; }

	constexpr bool CanExecuteWith() const
	{
		return Type() == at_void;
	}

	template< typename _param >
	constexpr bool CanExecuteWith( _param* val ) const
	{
		constexpr bool CanCast = std::is_same_v< _param*, decltype( thinker_cast< _param >( (_param*)nullptr ) ) >;
		return Type() == at_p1 && CanCast;
	}

	template< typename _param1, typename _param2 >
	constexpr bool CanExecuteWith( _param1* player, _param2* pspr ) const
	{
		constexpr bool IsPlayer = std::is_same_v< _param1*, player_t* >;
		constexpr bool IsPSpDef = std::is_same_v< _param2*, pspdef_t* >;

		return Type() == at_p2 && IsPlayer && IsPSpDef;
	}

private:
	constexpr actiontype_t Type() const
	{
		return (actiontype_t)(type & ~at_disabled);
	}

	union
	{
		actionf_v		_acv;
		actionf_p1		_acp1;
		actionf_p2		_acp2;
	};
	actiontype_t type;

} actionf_t;

struct thinker_s
{
	constexpr bool Enabled() const
	{
		return function.Enabled();
	}

	inline void Disable()
	{
		function.Disable();
	}

	inline void Enable()
	{
		function.Enable();
	}

	actionf_t			function;
	thinker_t*			prev;
	thinker_t*			next;
};
