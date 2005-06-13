-----------------------------------------------------------------------------
--
-- Static flags
--
-- Static flags can only be set once, on the command-line.  Inside GHC,
-- each static flag corresponds to a top-level value, usually of type Bool.
--
-- (c) The University of Glasgow 2005
--
-----------------------------------------------------------------------------

module StaticFlags (
	parseStaticFlags,
	staticFlags,

	-- Ways
	WayName(..), v_Ways, v_Build_tag, v_RTS_Build_tag,

	-- Output style options
	opt_PprUserLength,
	opt_PprStyle_Debug,

	-- profiling opts
	opt_AutoSccsOnAllToplevs,
	opt_AutoSccsOnExportedToplevs,
	opt_AutoSccsOnIndividualCafs,
	opt_SccProfilingOn,
	opt_DoTickyProfiling,

	-- language opts
	opt_DictsStrict,
        opt_MaxContextReductionDepth,
	opt_IrrefutableTuples,
	opt_Parallel,
	opt_SMP,
	opt_RuntimeTypes,
	opt_Flatten,

	-- optimisation opts
	opt_NoMethodSharing, 
	opt_NoStateHack,
	opt_LiberateCaseThreshold,
	opt_CprOff,
	opt_RulesOff,
	opt_SimplNoPreInlining,
	opt_SimplExcessPrecision,
	opt_MaxWorkerArgs,

	-- Unfolding control
	opt_UF_CreationThreshold,
	opt_UF_UseThreshold,
	opt_UF_FunAppDiscount,
	opt_UF_KeenessFactor,
	opt_UF_UpdateInPlace,
	opt_UF_DearOp,

	-- misc opts
	opt_IgnoreDotGhci,
	opt_ErrorSpans,
	opt_EmitCExternDecls,
	opt_GranMacros,
	opt_HiVersion,
	opt_HistorySize,
	opt_OmitBlackHoling,
	opt_Static,
	opt_Unregisterised,
	opt_EmitExternalCore,
	opt_PIC,
	v_Ld_inputs,
  ) where

#include "HsVersions.h"

import Util		( consIORef )
import CmdLineParser
import Config		( cProjectVersionInt, cProjectPatchLevel,
			  cGhcUnregisterised )
import FastString	( FastString, mkFastString )
import Util
import Maybes		( firstJust )
import Panic		( GhcException(..), ghcError )
import Constants	( mAX_CONTEXT_REDUCTION_DEPTH )

import EXCEPTION	( throwDyn )
import DATA_IOREF
import UNSAFE_IO	( unsafePerformIO )
import Monad		( when )
import Char		( isDigit )
import List		( sort, intersperse )

-----------------------------------------------------------------------------
-- Static flags

parseStaticFlags :: [String] -> IO [String]
parseStaticFlags args = do
  (leftover, errs) <- processArgs static_flags args
  when (not (null errs)) $ throwDyn (UsageError (unlines errs))

    -- deal with the way flags: the way (eg. prof) gives rise to
    -- futher flags, some of which might be static.
  way_flags <- findBuildTag

    -- if we're unregisterised, add some more flags
  let unreg_flags | cGhcUnregisterised == "YES" = unregFlags
		  | otherwise = []

  (more_leftover, errs) <- processArgs static_flags (unreg_flags ++ way_flags)
  when (not (null errs)) $ ghcError (UsageError (unlines errs))
  return (more_leftover++leftover)


-- note that ordering is important in the following list: any flag which
-- is a prefix flag (i.e. HasArg, Prefix, OptPrefix, AnySuffix) will override
-- flags further down the list with the same prefix.

static_flags :: [(String, OptKind IO)]
static_flags = [
	------- GHCi -------------------------------------------------------
     ( "ignore-dot-ghci", PassFlag addOpt )
  ,  ( "read-dot-ghci"  , NoArg (removeOpt "-ignore-dot-ghci") )

	------- ways --------------------------------------------------------
  ,  ( "prof"		, NoArg (addWay WayProf) )
  ,  ( "unreg"		, NoArg (addWay WayUnreg) )
  ,  ( "ticky"		, NoArg (addWay WayTicky) )
  ,  ( "parallel"	, NoArg (addWay WayPar) )
  ,  ( "gransim"	, NoArg (addWay WayGran) )
  ,  ( "smp"		, NoArg (addWay WaySMP) )
  ,  ( "debug"		, NoArg (addWay WayDebug) )
  ,  ( "ndp"		, NoArg (addWay WayNDP) )
  ,  ( "threaded"	, NoArg (addWay WayThreaded) )
 	-- ToDo: user ways

	------ Debugging ----------------------------------------------------
  ,  ( "dppr-noprags",     PassFlag addOpt )
  ,  ( "dppr-debug",       PassFlag addOpt )
  ,  ( "dppr-user-length", AnySuffix addOpt )
      -- rest of the debugging flags are dynamic

	--------- Profiling --------------------------------------------------
  ,  ( "auto-all"	, NoArg (addOpt "-fauto-sccs-on-all-toplevs") )
  ,  ( "auto"		, NoArg (addOpt "-fauto-sccs-on-exported-toplevs") )
  ,  ( "caf-all"	, NoArg (addOpt "-fauto-sccs-on-individual-cafs") )
         -- "ignore-sccs"  doesn't work  (ToDo)

  ,  ( "no-auto-all"	, NoArg (removeOpt "-fauto-sccs-on-all-toplevs") )
  ,  ( "no-auto"	, NoArg (removeOpt "-fauto-sccs-on-exported-toplevs") )
  ,  ( "no-caf-all"	, NoArg (removeOpt "-fauto-sccs-on-individual-cafs") )

	------- Miscellaneous -----------------------------------------------
  ,  ( "no-link-chk"    , NoArg (return ()) ) -- ignored for backwards compat

	----- Linker --------------------------------------------------------
  ,  ( "static" 	, PassFlag addOpt )
  ,  ( "dynamic"        , NoArg (removeOpt "-static") )
  ,  ( "rdynamic"       , NoArg (return ()) ) -- ignored for compat w/ gcc

	----- RTS opts ------------------------------------------------------
  ,  ( "H"                 , HasArg (setHeapSize . fromIntegral . decodeSize) )
  ,  ( "Rghc-timing"	   , NoArg  (enableTimingStats) )

        ------ Compiler flags -----------------------------------------------
	-- All other "-fno-<blah>" options cancel out "-f<blah>" on the hsc cmdline
  ,  ( "fno-",			PrefixPred (\s -> isStaticFlag ("f"++s))
				    (\s -> removeOpt ("-f"++s)) )

	-- Pass all remaining "-f<blah>" options to hsc
  ,  ( "f", 			AnySuffixPred (isStaticFlag) addOpt )
  ]

addOpt = consIORef v_opt_C

addWay = consIORef v_Ways

removeOpt f = do
  fs <- readIORef v_opt_C
  writeIORef v_opt_C $! filter (/= f) fs    

lookUp	       	 :: FastString -> Bool
lookup_def_int   :: String -> Int -> Int
lookup_def_float :: String -> Float -> Float
lookup_str       :: String -> Maybe String

-- holds the static opts while they're being collected, before
-- being unsafely read by unpacked_static_opts below.
GLOBAL_VAR(v_opt_C, defaultStaticOpts, [String])
staticFlags = unsafePerformIO (readIORef v_opt_C)

-- -static is the default
defaultStaticOpts = ["-static"]

packed_static_opts   = map mkFastString staticFlags

lookUp     sw = sw `elem` packed_static_opts
	
-- (lookup_str "foo") looks for the flag -foo=X or -fooX, 
-- and returns the string X
lookup_str sw 
   = case firstJust (map (startsWith sw) staticFlags) of
	Just ('=' : str) -> Just str
	Just str         -> Just str
	Nothing		 -> Nothing	

lookup_def_int sw def = case (lookup_str sw) of
			    Nothing -> def		-- Use default
		  	    Just xx -> try_read sw xx

lookup_def_float sw def = case (lookup_str sw) of
			    Nothing -> def		-- Use default
		  	    Just xx -> try_read sw xx


try_read :: Read a => String -> String -> a
-- (try_read sw str) tries to read s; if it fails, it
-- bleats about flag sw
try_read sw str
  = case reads str of
	((x,_):_) -> x	-- Be forgiving: ignore trailing goop, and alternative parses
	[]	  -> ghcError (UsageError ("Malformed argument " ++ str ++ " for flag " ++ sw))
			-- ToDo: hack alert. We should really parse the arugments
			-- 	 and announce errors in a more civilised way.


{-
 Putting the compiler options into temporary at-files
 may turn out to be necessary later on if we turn hsc into
 a pure Win32 application where I think there's a command-line
 length limit of 255. unpacked_opts understands the @ option.

unpacked_opts :: [String]
unpacked_opts =
  concat $
  map (expandAts) $
  map unpackFS argv  -- NOT ARGV any more: v_Static_hsc_opts
  where
   expandAts ('@':fname) = words (unsafePerformIO (readFile fname))
   expandAts l = [l]
-}


opt_IgnoreDotGhci		= lookUp FSLIT("-ignore-dot-ghci")

-- debugging opts
opt_PprStyle_Debug		= lookUp  FSLIT("-dppr-debug")
opt_PprUserLength	        = lookup_def_int "-dppr-user-length" 5 --ToDo: give this a name

-- profiling opts
opt_AutoSccsOnAllToplevs	= lookUp  FSLIT("-fauto-sccs-on-all-toplevs")
opt_AutoSccsOnExportedToplevs	= lookUp  FSLIT("-fauto-sccs-on-exported-toplevs")
opt_AutoSccsOnIndividualCafs	= lookUp  FSLIT("-fauto-sccs-on-individual-cafs")
opt_SccProfilingOn		= lookUp  FSLIT("-fscc-profiling")
opt_DoTickyProfiling		= lookUp  FSLIT("-fticky-ticky")

-- language opts
opt_DictsStrict			= lookUp  FSLIT("-fdicts-strict")
opt_IrrefutableTuples		= lookUp  FSLIT("-firrefutable-tuples")
opt_MaxContextReductionDepth	= lookup_def_int "-fcontext-stack" mAX_CONTEXT_REDUCTION_DEPTH
opt_Parallel			= lookUp  FSLIT("-fparallel")
opt_SMP				= lookUp  FSLIT("-fsmp")
opt_Flatten			= lookUp  FSLIT("-fflatten")

-- optimisation opts
opt_NoStateHack			= lookUp  FSLIT("-fno-state-hack")
opt_NoMethodSharing		= lookUp  FSLIT("-fno-method-sharing")
opt_CprOff			= lookUp  FSLIT("-fcpr-off")
opt_RulesOff			= lookUp  FSLIT("-frules-off")
	-- Switch off CPR analysis in the new demand analyser
opt_LiberateCaseThreshold	= lookup_def_int "-fliberate-case-threshold" (10::Int)
opt_MaxWorkerArgs		= lookup_def_int "-fmax-worker-args" (10::Int)

opt_EmitCExternDecls	        = lookUp  FSLIT("-femit-extern-decls")
opt_GranMacros			= lookUp  FSLIT("-fgransim")
opt_HiVersion			= read (cProjectVersionInt ++ cProjectPatchLevel) :: Int
opt_HistorySize			= lookup_def_int "-fhistory-size" 20
opt_OmitBlackHoling		= lookUp  FSLIT("-dno-black-holing")
opt_RuntimeTypes		= lookUp  FSLIT("-fruntime-types")

-- Simplifier switches
opt_SimplNoPreInlining		= lookUp  FSLIT("-fno-pre-inlining")
	-- NoPreInlining is there just to see how bad things
	-- get if you don't do it!
opt_SimplExcessPrecision	= lookUp  FSLIT("-fexcess-precision")

-- Unfolding control
opt_UF_CreationThreshold	= lookup_def_int "-funfolding-creation-threshold"  (45::Int)
opt_UF_UseThreshold		= lookup_def_int "-funfolding-use-threshold"	   (8::Int)	-- Discounts can be big
opt_UF_FunAppDiscount		= lookup_def_int "-funfolding-fun-discount"	   (6::Int)	-- It's great to inline a fn
opt_UF_KeenessFactor		= lookup_def_float "-funfolding-keeness-factor"	   (1.5::Float)
opt_UF_UpdateInPlace		= lookUp  FSLIT("-funfolding-update-in-place")

opt_UF_DearOp   = ( 4 :: Int)
			
opt_Static			= lookUp  FSLIT("-static")
opt_Unregisterised		= lookUp  FSLIT("-funregisterised")
opt_EmitExternalCore		= lookUp  FSLIT("-fext-core")

-- Include full span info in error messages, instead of just the start position.
opt_ErrorSpans			= lookUp FSLIT("-ferror-spans")

opt_PIC                         = lookUp FSLIT("-fPIC")

-- object files and libraries to be linked in are collected here.
-- ToDo: perhaps this could be done without a global, it wasn't obvious
-- how to do it though --SDM.
GLOBAL_VAR(v_Ld_inputs,	[],      [String])

isStaticFlag f =
  f `elem` [
	"fauto-sccs-on-all-toplevs",
	"fauto-sccs-on-exported-toplevs",
	"fauto-sccs-on-individual-cafs",
	"fscc-profiling",
	"fticky-ticky",
	"fall-strict",
	"fdicts-strict",
	"firrefutable-tuples",
	"fparallel",
	"fsmp",
	"fflatten",
	"fsemi-tagging",
	"flet-no-escape",
	"femit-extern-decls",
	"fglobalise-toplev-names",
	"fgransim",
	"fno-hi-version-check",
	"dno-black-holing",
	"fno-method-sharing",
	"fno-state-hack",
	"fruntime-types",
	"fno-pre-inlining",
	"fexcess-precision",
	"funfolding-update-in-place",
	"static",
	"funregisterised",
	"fext-core",
	"frule-check",
	"frules-off",
	"fcpr-off",
	"ferror-spans",
	"fPIC"
	]
  || any (flip prefixMatch f) [
	"fcontext-stack",
	"fliberate-case-threshold",
	"fmax-worker-args",
	"fhistory-size",
	"funfolding-creation-threshold",
	"funfolding-use-threshold",
	"funfolding-fun-discount",
	"funfolding-keeness-factor"
     ]



-- Misc functions for command-line options

startsWith :: String -> String -> Maybe String
-- startsWith pfx (pfx++rest) = Just rest

startsWith []     str = Just str
startsWith (c:cs) (s:ss)
  = if c /= s then Nothing else startsWith cs ss
startsWith  _	  []  = Nothing


-----------------------------------------------------------------------------
-- convert sizes like "3.5M" into integers

decodeSize :: String -> Integer
decodeSize str
  | c == ""		 = truncate n
  | c == "K" || c == "k" = truncate (n * 1000)
  | c == "M" || c == "m" = truncate (n * 1000 * 1000)
  | c == "G" || c == "g" = truncate (n * 1000 * 1000 * 1000)
  | otherwise            = throwDyn (CmdLineError ("can't decode size: " ++ str))
  where (m, c) = span pred str
        n      = read m  :: Double
	pred c = isDigit c || c == '.'


-----------------------------------------------------------------------------
-- RTS Hooks

#if __GLASGOW_HASKELL__ >= 504
foreign import ccall unsafe "setHeapSize"       setHeapSize       :: Int -> IO ()
foreign import ccall unsafe "enableTimingStats" enableTimingStats :: IO ()
#else
foreign import "setHeapSize"       unsafe setHeapSize       :: Int -> IO ()
foreign import "enableTimingStats" unsafe enableTimingStats :: IO ()
#endif

-----------------------------------------------------------------------------
-- Ways

-- The central concept of a "way" is that all objects in a given
-- program must be compiled in the same "way".  Certain options change
-- parameters of the virtual machine, eg. profiling adds an extra word
-- to the object header, so profiling objects cannot be linked with
-- non-profiling objects.

-- After parsing the command-line options, we determine which "way" we
-- are building - this might be a combination way, eg. profiling+ticky-ticky.

-- We then find the "build-tag" associated with this way, and this
-- becomes the suffix used to find .hi files and libraries used in
-- this compilation.

GLOBAL_VAR(v_Build_tag, "", String)

-- The RTS has its own build tag, because there are some ways that
-- affect the RTS only.
GLOBAL_VAR(v_RTS_Build_tag, "", String)

data WayName
  = WayThreaded
  | WayDebug
  | WayProf
  | WayUnreg
  | WayTicky
  | WayPar
  | WayGran
  | WaySMP
  | WayNDP
  | WayUser_a
  | WayUser_b
  | WayUser_c
  | WayUser_d
  | WayUser_e
  | WayUser_f
  | WayUser_g
  | WayUser_h
  | WayUser_i
  | WayUser_j
  | WayUser_k
  | WayUser_l
  | WayUser_m
  | WayUser_n
  | WayUser_o
  | WayUser_A
  | WayUser_B
  deriving (Eq,Ord)

GLOBAL_VAR(v_Ways, [] ,[WayName])

allowed_combination way = and [ x `allowedWith` y 
			      | x <- way, y <- way, x < y ]
  where
	-- Note ordering in these tests: the left argument is
	-- <= the right argument, according to the Ord instance
	-- on Way above.

	-- debug is allowed with everything
	_ `allowedWith` WayDebug		= True
	WayDebug `allowedWith` _		= True

	WayThreaded `allowedWith` WayProf	= True
	WayProf `allowedWith` WayUnreg		= True
	WayProf `allowedWith` WaySMP		= True
	WayProf `allowedWith` WayNDP		= True
	_ `allowedWith` _ 			= False


findBuildTag :: IO [String]  -- new options
findBuildTag = do
  way_names <- readIORef v_Ways
  let ws = sort way_names
  if not (allowed_combination ws)
      then throwDyn (CmdLineError $
      		    "combination not supported: "  ++
      		    foldr1 (\a b -> a ++ '/':b) 
      		    (map (wayName . lkupWay) ws))
      else let ways    = map lkupWay ws
      	       tag     = mkBuildTag (filter (not.wayRTSOnly) ways)
      	       rts_tag = mkBuildTag ways
      	       flags   = map wayOpts ways
      	   in do
      	   writeIORef v_Build_tag tag
      	   writeIORef v_RTS_Build_tag rts_tag
      	   return (concat flags)

mkBuildTag :: [Way] -> String
mkBuildTag ways = concat (intersperse "_" (map wayTag ways))

lkupWay w = 
   case lookup w way_details of
	Nothing -> error "findBuildTag"
	Just details -> details

data Way = Way {
  wayTag     :: String,
  wayRTSOnly :: Bool,
  wayName    :: String,
  wayOpts    :: [String]
  }

way_details :: [ (WayName, Way) ]
way_details =
  [ (WayThreaded, Way "thr" True "Threaded" [
#if defined(freebsd_TARGET_OS)
	  "-optc-pthread"
        , "-optl-pthread"
#endif
	] ),

    (WayDebug, Way "debug" True "Debug" [] ),

    (WayProf, Way  "p" False "Profiling"
	[ "-fscc-profiling"
	, "-DPROFILING"
	, "-optc-DPROFILING" ]),

    (WayTicky, Way  "t" False "Ticky-ticky Profiling"  
	[ "-fticky-ticky"
	, "-DTICKY_TICKY"
	, "-optc-DTICKY_TICKY" ]),

    (WayUnreg, Way  "u" False "Unregisterised" 
	unregFlags ),

    -- optl's below to tell linker where to find the PVM library -- HWL
    (WayPar, Way  "mp" False "Parallel" 
	[ "-fparallel"
	, "-D__PARALLEL_HASKELL__"
	, "-optc-DPAR"
	, "-package concurrent"
        , "-optc-w"
        , "-optl-L${PVM_ROOT}/lib/${PVM_ARCH}"
        , "-optl-lpvm3"
        , "-optl-lgpvm3" ]),

    -- at the moment we only change the RTS and could share compiler and libs!
    (WayPar, Way  "mt" False "Parallel ticky profiling" 
	[ "-fparallel"
	, "-D__PARALLEL_HASKELL__"
	, "-optc-DPAR"
	, "-optc-DPAR_TICKY"
	, "-package concurrent"
        , "-optc-w"
        , "-optl-L${PVM_ROOT}/lib/${PVM_ARCH}"
        , "-optl-lpvm3"
        , "-optl-lgpvm3" ]),

    (WayPar, Way  "md" False "Distributed" 
	[ "-fparallel"
	, "-D__PARALLEL_HASKELL__"
	, "-D__DISTRIBUTED_HASKELL__"
	, "-optc-DPAR"
	, "-optc-DDIST"
	, "-package concurrent"
        , "-optc-w"
        , "-optl-L${PVM_ROOT}/lib/${PVM_ARCH}"
        , "-optl-lpvm3"
        , "-optl-lgpvm3" ]),

    (WayGran, Way  "mg" False "GranSim"
	[ "-fgransim"
	, "-D__GRANSIM__"
	, "-optc-DGRAN"
	, "-package concurrent" ]),

    (WaySMP, Way  "s" False "SMP"
	[ "-fsmp"
#if !defined(mingw32_TARGET_OS)
	, "-optc-pthread"
#endif
#if !defined(mingw32_TARGET_OS) && !defined(freebsd_TARGET_OS)
	, "-optl-pthread"
#endif
	, "-optc-DSMP" ]),

    (WayNDP, Way  "ndp" False "Nested data parallelism"
	[ "-fparr"
	, "-fflatten"]),

    (WayUser_a,  Way  "a"  False "User way 'a'"  ["$WAY_a_REAL_OPTS"]),	
    (WayUser_b,  Way  "b"  False "User way 'b'"  ["$WAY_b_REAL_OPTS"]),	
    (WayUser_c,  Way  "c"  False "User way 'c'"  ["$WAY_c_REAL_OPTS"]),	
    (WayUser_d,  Way  "d"  False "User way 'd'"  ["$WAY_d_REAL_OPTS"]),	
    (WayUser_e,  Way  "e"  False "User way 'e'"  ["$WAY_e_REAL_OPTS"]),	
    (WayUser_f,  Way  "f"  False "User way 'f'"  ["$WAY_f_REAL_OPTS"]),	
    (WayUser_g,  Way  "g"  False "User way 'g'"  ["$WAY_g_REAL_OPTS"]),	
    (WayUser_h,  Way  "h"  False "User way 'h'"  ["$WAY_h_REAL_OPTS"]),	
    (WayUser_i,  Way  "i"  False "User way 'i'"  ["$WAY_i_REAL_OPTS"]),	
    (WayUser_j,  Way  "j"  False "User way 'j'"  ["$WAY_j_REAL_OPTS"]),	
    (WayUser_k,  Way  "k"  False "User way 'k'"  ["$WAY_k_REAL_OPTS"]),	
    (WayUser_l,  Way  "l"  False "User way 'l'"  ["$WAY_l_REAL_OPTS"]),	
    (WayUser_m,  Way  "m"  False "User way 'm'"  ["$WAY_m_REAL_OPTS"]),	
    (WayUser_n,  Way  "n"  False "User way 'n'"  ["$WAY_n_REAL_OPTS"]),	
    (WayUser_o,  Way  "o"  False "User way 'o'"  ["$WAY_o_REAL_OPTS"]),	
    (WayUser_A,  Way  "A"  False "User way 'A'"  ["$WAY_A_REAL_OPTS"]),	
    (WayUser_B,  Way  "B"  False "User way 'B'"  ["$WAY_B_REAL_OPTS"]) 
  ]

unregFlags = 
   [ "-optc-DNO_REGS"
   , "-optc-DUSE_MINIINTERPRETER"
   , "-fno-asm-mangling"
   , "-funregisterised"
   , "-fvia-C" ]
