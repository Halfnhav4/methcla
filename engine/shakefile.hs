-- Copyright 2012-2013 Samplecount S.L.
-- 
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
-- 
--     http://www.apache.org/licenses/LICENSE-2.0
-- 
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

{-# LANGUAGE Rank2Types #-}
{-# LANGUAGE TemplateHaskell #-}

import           Control.Applicative ((<$>))
import           Control.Lens hiding (Action, (<.>))
import           Control.Monad
import           Data.Monoid
import           Development.Shake as Shake
import           Development.Shake.FilePath
import           Data.Char (toLower)
import           Data.List (intercalate, intersperse, isInfixOf, isSuffixOf)
import           Data.List.Split (splitOn)
import           Data.Maybe
import           GHC.Conc (numCapabilities)
import qualified System.Console.CmdArgs.Implicit as C
import           System.Console.CmdArgs.Explicit
import qualified System.Directory as Dir
import           System.Environment
import           System.FilePath.Find
import           System.IO
import           System.Process (readProcess)

import Debug.Trace

{-# DEPRECATE ^$ #-}
(^$) = flip (^.)

under :: FilePath -> [FilePath] -> [FilePath]
under dir = map prepend
    where prepend ""   = dir
          prepend "."  = dir
          prepend ".." = takeDirectory dir
          prepend x    = combine dir x

flag_ :: String -> String -> [String]
flag_ o x = [o, x]

flag :: String -> [String]
flag f = [f]

flags_ :: String -> [String] -> [String]
flags_ o = concat . map (flag_ o)

flags :: String -> [String] -> [String]
flags f = map (f++)

-- Lens utils
append l n = over l (`mappend` n)
prepend l n = over l (n `mappend`)

-- Shake utils
(?=>) :: FilePath -> (FilePath -> Shake.Action ()) -> Rules ()
f ?=> a = (equalFilePath f) ?> a

systemLoud :: FilePath -> [String] -> Shake.Action ()
systemLoud cmd args = do
    putQuiet $ unwords $ [cmd] ++ args
    system' cmd args

data Env = Env {
    _buildConfiguration :: String
  , _buildPrefix :: FilePath
  } deriving (Show)

makeLenses ''Env

mkEnv :: String -> FilePath -> Env
mkEnv = Env

data Target =
    IOS
  | IOS_Simulator
  | MacOSX
  deriving (Eq, Show)

targetString :: Target -> String
targetString = map toLower . show


data CTarget = CTarget {
    _buildTarget :: Target
  , _buildArch :: String
  } deriving (Show)

makeLenses ''CTarget

mkCTarget :: Target -> String -> CTarget
mkCTarget target arch = CTarget target arch

buildDir :: Env -> CTarget -> FilePath
buildDir env target =
      (env ^. buildPrefix)
  </> map toLower (env ^. buildConfiguration)
  </> (targetString (target ^. buildTarget))
  </> (target ^. buildArch)


data CLanguage = C | Cpp | ObjC | ObjCpp
                 deriving (Enum, Eq, Show)

data CABI = CABI | CppABI

data LinkResult = Executable
                | SharedLibrary
                | DynamicLibrary
                deriving (Enum, Eq, Show)

defaultCLanguageMap :: [(String, CLanguage)]
defaultCLanguageMap = concatMap f [
    ([".c"], C)
  , ([".cc", ".cpp", ".C"], Cpp)
  , ([".m"], ObjC)
  , ([".mm"], ObjCpp)
  ]
  where f (es, l) = map (flip (,) l) es

languageOf :: FilePath -> Maybe CLanguage
languageOf = flip lookup defaultCLanguageMap . takeExtension

data CBuildFlags = CBuildFlags {
    _systemIncludes :: [FilePath]
  , _userIncludes :: [FilePath]
  , _defines :: [(String, Maybe String)]
  , _preprocessorFlags :: [String]
  , _compilerFlags :: [(Maybe CLanguage, [String])]
  , _libraryPath :: [FilePath]
  , _libraries :: [String]
  , _linkerFlags :: [String]
  , _archiverFlags :: [String]
  } deriving (Show)

makeLenses ''CBuildFlags

type Linker = CTarget -> CToolChain -> CBuildFlags -> [FilePath] -> FilePath -> Shake.Action ()
type Archiver = Linker

data CToolChain = CToolChain {
    _prefix :: Maybe FilePath
  , _compilerCmd :: String
  , _archiverCmd :: String
  , _archiver :: Archiver
  , _linkerCmd :: String
  , _linker :: LinkResult -> Linker
  }

makeLenses ''CToolChain

defaultArchiver :: Archiver
defaultArchiver target toolChain buildFlags inputs output = do
    need inputs
    systemLoud (tool archiverCmd toolChain)
        $ buildFlags ^. archiverFlags
        ++ [output]
        ++ inputs

defaultLinker :: Linker
defaultLinker target toolChain buildFlags inputs output = do
    need inputs
    systemLoud (tool linkerCmd toolChain)
          $  buildFlags ^. linkerFlags
          ++ flags "-L" (libraryPath ^$ buildFlags)
          ++ flags "-l" (libraries ^$ buildFlags)
          ++ flag_ "-o" output
          ++ inputs

defaultCToolChain :: CToolChain
defaultCToolChain =
    CToolChain {
        _prefix = Nothing
      , _compilerCmd = "gcc"
      , _archiverCmd = "ar"
      , _archiver = defaultArchiver
      , _linkerCmd = "gcc"
      , _linker = \link target toolChain ->
            case link of
                Executable -> defaultLinker target toolChain
                _          -> defaultLinker target toolChain . append linkerFlags (flag "-shared")
      }

tool :: (Getter CToolChain String) -> CToolChain -> FilePath
tool f toolChain = maybe cmd (flip combine ("bin" </> cmd))
                         (prefix ^$ toolChain)
    where cmd = f ^$ toolChain



defaultCBuildFlags :: CBuildFlags
defaultCBuildFlags =
    CBuildFlags {
        _systemIncludes = []
      , _userIncludes = []
      , _defines = []
      , _preprocessorFlags = []
      , _compilerFlags = []
      , _libraryPath = []
      , _libraries = []
      , _linkerFlags = []
      , _archiverFlags = []
      }

defineFlags :: CBuildFlags -> [String]
defineFlags = flags "-D" . map (\(a, b) -> maybe a (\b -> a++"="++b) b) . flip (^.) defines

compilerFlagsFor :: Maybe CLanguage -> CBuildFlags -> [String]
compilerFlagsFor lang = concat
                      . maybe (map snd . filter (isNothing.fst))
                              (mapMaybe . f) lang
                      . flip (^.) compilerFlags
    where f l (Nothing, x) = Just x
          f l (Just l', x) | l == l' = Just x
                           | otherwise = Nothing

type CBuildEnv = (CToolChain, CBuildFlags)


sed :: String -> FilePath -> FilePath -> Shake.Action ()
sed command input output = do
    need [input]
    (stdout, stderr) <- systemOutput "sed" ["-e", command, input]
    writeFile' output stdout

sourceTransform :: (FilePath -> FilePath) -> String -> FilePath -> Rules FilePath
sourceTransform f cmd input = do
    let output = f input
    output ?=> sed cmd input
    want [output]
    return output

dependencyFile :: CTarget -> CToolChain -> CBuildFlags -> FilePath -> FilePath -> Rules ()
dependencyFile target toolChain buildFlags input output = do
    output ?=> \_ -> do
        need [input]
        systemLoud (tool compilerCmd toolChain)
                $  flag_ "-arch" (buildArch ^$ target)
                ++ flags "-I" (systemIncludes ^$ buildFlags)
                ++ flags_ "-iquote" (userIncludes ^$ buildFlags)
                ++ (defineFlags buildFlags)
                ++ (preprocessorFlags ^$ buildFlags)
                ++ (compilerFlagsFor (languageOf input) buildFlags)
                ++ ["-MM", "-o", output, input]

parseDependencies :: String -> [FilePath]
parseDependencies = drop 2 . words . filter (/= '\\')

type ObjectRule = CTarget -> CToolChain -> CBuildFlags -> FilePath -> [FilePath] -> FilePath -> Rules ()

staticObject :: ObjectRule
staticObject target toolChain buildFlags input deps output = do
    let depFile = output <.> "d"
    dependencyFile target toolChain buildFlags input depFile
    output ?=> \_ ->  do
        deps' <- parseDependencies <$> readFile' depFile
        need $ [input] ++ deps ++ deps'
        systemLoud (tool compilerCmd toolChain)
                $  flag_ "-arch" (buildArch ^$ target)
                ++ flags "-I" (systemIncludes ^$ buildFlags)
                ++ flags_ "-iquote" (userIncludes ^$ buildFlags)
                ++ (defineFlags buildFlags)
                ++ (preprocessorFlags ^$ buildFlags)
                ++ (compilerFlagsFor (languageOf input) buildFlags)
                ++ ["-c", "-o", output, input]

sharedObject :: ObjectRule
sharedObject target toolChain = staticObject target toolChain . append compilerFlags [(Nothing, flag "-fPIC")]

data SourceTree b = SourceTree (b -> b) [(FilePath, [FilePath])]

sourceTree :: (b -> b) -> [(FilePath, [FilePath])] -> SourceTree b
sourceTree = SourceTree

sourceFiles :: [FilePath] -> [(FilePath, [FilePath])]
sourceFiles = map (flip (,) [])

data Library = Library {
    libName :: String
  , libSources :: [SourceTree CBuildFlags]
  }

staticLibFileName :: String -> FilePath
staticLibFileName = ("lib"++) . (<.> "a")

sharedLibFileName :: String -> FilePath
sharedLibFileName = ("lib"++) . (<.> "dylib")

libBuildDir :: Env -> CTarget -> FilePath -> FilePath
libBuildDir env target libFileName = buildDir env target </> map tr (libFileName)
    where tr '.' = '_'
          tr x   = x

libBuildPath :: Env -> CTarget -> FilePath -> FilePath
libBuildPath env target libFileName = buildDir env target </> libFileName

cLibrary :: ObjectRule -> Linker -> (Library -> FilePath)
         -> Env -> CTarget -> CToolChain -> CBuildFlags
         -> Library
         -> Rules FilePath
cLibrary object link libFileName env target toolChain buildFlags lib = do
    let libFile = libFileName lib
        libPath = libBuildPath env target libFile
        buildDir = libBuildDir env target libFile
    objects <- forM (libSources lib) $ \(SourceTree mapBuildFlags srcTree) -> do
        let src = map fst srcTree
            dep = map snd srcTree
            obj = map (combine buildDir . makeRelative buildDir . (<.> "o")) src
        zipWithM_ ($) (zipWith (object target toolChain (mapBuildFlags buildFlags)) src dep) obj
        return obj
    libPath ?=> link target toolChain buildFlags (concat objects)
    return libPath

staticLibrary :: Env -> CTarget -> CToolChain -> CBuildFlags -> Library -> Rules FilePath
staticLibrary env target toolChain =
    cLibrary
        staticObject
        (archiver ^$ toolChain)
        (staticLibFileName . libName)
        env target toolChain

sharedLibrary :: Env -> CTarget -> CToolChain -> CBuildFlags -> Library -> Rules FilePath
sharedLibrary env target toolChain =
    cLibrary
        sharedObject
        ((linker ^$ toolChain) SharedLibrary)
        (sharedLibFileName . libName)
        env target toolChain

-- ====================================================================
-- Target and build defaults

osxArchiver :: Archiver
osxArchiver target toolChain buildFlags inputs output = do
    need inputs
    systemLoud (tool archiverCmd toolChain)
          $  buildFlags ^. archiverFlags
          ++ flag "-static"
          ++ flag_ "-o" output
          ++ inputs

osxLinker :: LinkResult -> Linker
osxLinker link target toolChain =
    case link of
        Executable     -> defaultLinker target toolChain
        SharedLibrary  -> defaultLinker target toolChain . prepend linkerFlags (flag "-dynamiclib")
        DynamicLibrary -> defaultLinker target toolChain . prepend linkerFlags (flag "-bundle")

newtype DeveloperPath = DeveloperPath { developerPath :: FilePath }

-- | Get base path of development tools on OSX.
getDeveloperPath :: IO DeveloperPath
getDeveloperPath =
  (DeveloperPath . head . splitOn "\n")
    <$> readProcess "xcode-select" ["--print-path"] ""

platformDeveloperPath :: DeveloperPath -> String -> FilePath
platformDeveloperPath developer platform =
  developerPath developer </> "Platforms" </> (platform ++ ".platform") </> "Developer"

platformSDKPath :: DeveloperPath -> String -> String -> FilePath
platformSDKPath developer platform sdkVersion =
  platformDeveloperPath developer platform </> "SDKs" </> (platform ++ sdkVersion ++ ".sdk")

-- | Get OSX system version (first two digits).
getSystemVersion :: IO String
getSystemVersion =
  (intercalate "." . take 2 . splitOn ".")
    <$> readProcess "sw_vers" ["-productVersion"] ""

cToolChain_MacOSX :: DeveloperPath -> CToolChain
cToolChain_MacOSX developer =
    prefix .~ Just (developerPath developer </> "Toolchains/XcodeDefault.xctoolchain/usr")
  $ compilerCmd .~ "clang"
  $ archiverCmd .~ "libtool"
  $ archiver .~ osxArchiver
  $ linkerCmd .~ "clang++"
  $ linker .~ osxLinker
  $ defaultCToolChain

cToolChain_MacOSX_gcc :: DeveloperPath -> CToolChain
cToolChain_MacOSX_gcc developer =
    compilerCmd .~ "gcc"
  $ linkerCmd .~ "g++"
  $ cToolChain_MacOSX developer

cBuildFlags_MacOSX :: DeveloperPath -> String -> CBuildFlags
cBuildFlags_MacOSX developer sdkVersion =
    append preprocessorFlags [
      "-isysroot"
    , platformSDKPath developer "MacOSX" sdkVersion ]
  . append compilerFlags [(Nothing, flag ("-mmacosx-version-min=" ++ sdkVersion))]
  $ defaultCBuildFlags

iosMinVersion :: String
iosMinVersion = "50000"
--iosMinVersion = "40200"

cToolChain_IOS :: DeveloperPath -> CToolChain
cToolChain_IOS = cToolChain_MacOSX

cBuildFlags_IOS :: DeveloperPath -> String -> CBuildFlags
cBuildFlags_IOS developer sdkVersion =
    append defines [("__IPHONE_OS_VERSION_MIN_REQUIRED", Just iosMinVersion)]
  . append preprocessorFlags
            [ "-isysroot"
            , platformSDKPath developer "iPhoneOS" sdkVersion ]
  $ defaultCBuildFlags

cToolChain_IOS_Simulator :: DeveloperPath -> CToolChain
cToolChain_IOS_Simulator = cToolChain_MacOSX

cBuildFlags_IOS_Simulator :: DeveloperPath -> String -> CBuildFlags
cBuildFlags_IOS_Simulator developer sdkVersion =
    append defines [("__IPHONE_OS_VERSION_MIN_REQUIRED", Just iosMinVersion)]
  . append preprocessorFlags
            [ "-isysroot"
            , platformSDKPath developer "iPhoneSimulator" sdkVersion ]
  $ defaultCBuildFlags

-- ====================================================================
-- Library

externalLibraries :: FilePath
externalLibraries = "external_libraries"

externalLibrary :: FilePath -> FilePath
externalLibrary = combine externalLibraries

lv2Dir :: FilePath
lv2Dir = externalLibrary "lv2"

boostDir :: FilePath
boostDir = externalLibrary "boost"

serdDir :: FilePath
serdDir = externalLibrary "serd"

serdBuildFlags :: CBuildFlags -> CBuildFlags
serdBuildFlags = append userIncludes
                    [ serdDir, serdDir </> "src"
                    , externalLibraries ]

sordDir :: FilePath
sordDir = externalLibrary "sord"

sordBuildFlags :: CBuildFlags -> CBuildFlags
sordBuildFlags = append userIncludes
                    [ sordDir, sordDir </> "src"
                    , serdDir
                    , externalLibraries ]

sratomDir :: FilePath
sratomDir = externalLibrary "sratom"

sratomBuildFlags :: CBuildFlags -> CBuildFlags
sratomBuildFlags = append userIncludes
                    [ sratomDir
                    , serdDir
                    , sordDir
                    , externalLibraries
                    , lv2Dir ]

lilvDir :: FilePath
lilvDir = externalLibrary "lilv"

lilvBuildFlags :: CBuildFlags -> CBuildFlags
lilvBuildFlags = append userIncludes
                    [ lilvDir, lilvDir </> "src"
                    , serdDir
                    , sordDir
                    , sratomDir
                    , externalLibraries
                    , lv2Dir ]

boostBuildFlags :: CBuildFlags -> CBuildFlags
boostBuildFlags = append systemIncludes [ boostDir ]

engineBuildFlags :: CTarget -> CBuildFlags -> CBuildFlags
engineBuildFlags target =
    append userIncludes
      ( ["."]
     ++ [ "external_libraries" ]
     ++ [ "external_libraries/lv2" ]
     ++ case buildTarget ^$ target of
            IOS           -> [ "platform/ios" ]
            IOS_Simulator -> [ "platform/ios" ]
            MacOSX        -> [ "platform/jack" ]
            -- _             -> []
     ++ [ serdDir, sordDir, lilvDir ] )
  . append systemIncludes
       ( [ "include", "src" ]
      ++ [ boostDir
         , "external_libraries/boost_lockfree" ] )

-- | Build flags common to all targets
methclaCommonBuildFlags :: CBuildFlags -> CBuildFlags
methclaCommonBuildFlags = append compilerFlags [
    (Just C, flag "-std=c11")
  , (Just Cpp, flag "-std=c++11" ++ flag "-stdlib=libc++")
  , (Nothing, flag "-Wall")
  , (Nothing, flag "-fvisibility=hidden")
  , (Just Cpp, flag "-fvisibility-inlines-hidden")
  ]

-- | Build flags for static library
methclaStaticBuidFlags :: CBuildFlags -> CBuildFlags
methclaStaticBuidFlags = id

-- | Build flags for shared library
methclaSharedBuildFlags :: CBuildFlags -> CBuildFlags
methclaSharedBuildFlags = libraries .~ [ "m" ]

methclaLib :: CTarget -> IO Library
methclaLib target = do
    --boostSrc <- find always
    --                (    extension ==? ".cpp"
    --                 &&? (not . isSuffixOf "win32") <$> directory
    --                 &&? (not . isSuffixOf "test/src") <$> directory
    --                 -- FIXME: linking `filesystem' into a shared library fails with:
    --                 --     Undefined symbols for architecture x86_64:
    --                 --       "vtable for boost::filesystem::detail::utf8_codecvt_facet", referenced from:
    --                 --           boost::filesystem::detail::utf8_codecvt_facet::utf8_codecvt_facet(unsigned long) in v2_path.cpp.o
    --                 --           boost::filesystem::detail::utf8_codecvt_facet::utf8_codecvt_facet(unsigned long) in path.cpp.o
    --                 --       NOTE: a missing vtable usually means the first non-inline virtual member function has no definition.
    --                 &&? (not . elem "filesystem/" . splitPath) <$> filePath
    --                 &&? (not . elem "thread/" . splitPath) <$> filePath
    --                 &&? (fileName /=? "utf8_codecvt_facet.cpp")
    --                )
    --                boostDir
    return $ Library "methcla" $ [
        -- serd
        sourceTree serdBuildFlags $ sourceFiles $
            under (serdDir </> "src") [
                "env.c"
              , "node.c"
              , "reader.c"
              , "string.c"
              , "uri.c"
              , "writer.c"
              ]
        -- sord
      , sourceTree sordBuildFlags $ sourceFiles $
            under (sordDir </> "src") [
                "sord.c"
              , "syntax.c"
              , "zix/digest.c"
              , "zix/hash.c"
              , "zix/tree.c"
              ]
        -- sratom
      , sourceTree sratomBuildFlags $ sourceFiles $
            under (sratomDir </> "src") [
                "sratom.c"
              ]
        -- lilv
      , sourceTree lilvBuildFlags $ sourceFiles $
            under (lilvDir </> "src") [
                "collections.c"
              , "instance.c"
              , "lib.c"
              , "node.c"
              , "plugin.c"
              , "pluginclass.c"
              , "port.c"
              , "query.c"
              , "scalepoint.c"
              , "state.c"
              , "ui.c"
              , "util.c"
              , "world.c"
              -- FIXME: Make sure during compilation that this is actually the same as sord/src/zix/tree.c?
              --, "zix/tree.c"
              ]
        -- boost
      , sourceTree boostBuildFlags $ sourceFiles $
            under (boostDir </> "libs") [
                "date_time/src/gregorian/date_generators.cpp"
              , "date_time/src/gregorian/greg_month.cpp"
              , "date_time/src/gregorian/greg_weekday.cpp"
              , "date_time/src/gregorian/gregorian_types.cpp"
              , "date_time/src/posix_time/posix_time_types.cpp"
              , "exception/src/clone_current_exception_non_intrusive.cpp"
              , "filesystem/src/codecvt_error_category.cpp"
              , "filesystem/src/operations.cpp"
              , "filesystem/src/path.cpp"
              , "filesystem/src/path_traits.cpp"
              , "filesystem/src/portability.cpp"
              , "filesystem/src/unique_path.cpp"
              --, "filesystem/src/utf8_codecvt_facet.cpp"
              , "filesystem/src/windows_file_codecvt.cpp"
              , "system/src/error_code.cpp"
              ]
        -- engine
      , sourceTree (engineBuildFlags target) $ sourceFiles $
            under "src" [
                "Methcla/API.cpp"
              , "Methcla/Audio/AudioBus.cpp"
              , "Methcla/Audio/Client.cpp"
              , "Methcla/Audio/Engine.cpp"
              , "Methcla/Audio/Group.cpp"
              , "Methcla/Audio/Node.cpp"
              , "Methcla/Audio/Resource.cpp"
              , "Methcla/Audio/Synth.cpp"
              , "Methcla/Audio/SynthDef.cpp"
              , "Methcla/LV2/URIDMap.cpp"
              , "Methcla/Memory/Manager.cpp"
              , "Methcla/Memory.cpp"
              , "Methcla/Plugin/Loader.cpp"
              ]
            ++ [ "external_libraries/zix/ring.c" ]
            -- plugins
            ++ [ "lv2/methc.la/plugins/sine.lv2/sine.cpp" ]
            -- platform dependent
            ++ (if (buildTarget ^$ target) `elem` [IOS, IOS_Simulator]
                then under "platform/ios" [ "Methcla/Audio/IO/RemoteIODriver.cpp" ]
                else if (buildTarget ^$ target) `elem` [MacOSX]
                     then under "platform/jack" [ "Methcla/Audio/IO/JackDriver.cpp" ]
                     else [])
        ]

-- ====================================================================
-- Configurations

type Configuration = (String, CBuildFlags -> CBuildFlags)

applyConfiguration :: String -> [Configuration] -> CBuildFlags -> CBuildFlags
applyConfiguration c cs =
    case lookup c cs of
        Nothing -> id
        Just f  -> f

applyBuildConfiguration :: Env -> [Configuration] -> CBuildFlags -> CBuildFlags
applyBuildConfiguration env = applyConfiguration (buildConfiguration ^$ env)

configurations :: [Configuration]
configurations = [
    ( "release",
        append compilerFlags [(Nothing, flag "-O2")]
      . append defines [("NDEBUG", Nothing)]
    )
  , ( "debug",
        append compilerFlags [(Nothing, flag "-O0" ++ flag "-gdwarf-2")]
    )
  ]

-- ====================================================================
-- PkgConfig

pkgConfig :: String -> IO (CBuildFlags -> CBuildFlags)
pkgConfig pkg = do
    cflags <- parseFlags <$> readProcess "pkg-config" ["--cflags", pkg] ""
    lflags <- parseFlags <$> readProcess "pkg-config" ["--libs", pkg] ""
    return $ append compilerFlags [(Nothing, cflags)] . append linkerFlags lflags
    where
        parseFlags = map (dropSuffix "\\") . words . head . lines
        dropSuffix s x = if s `isSuffixOf` x
                         then reverse (drop (length s) (reverse x))
                         else x

-- ====================================================================
-- Commandline options

getShakeOptions :: FilePath -> IO ShakeOptions
getShakeOptions buildDir = do
    nc <- return numCapabilities -- This has been changed to Control.Concurrent.getNumCapabilities in 7.?
    return $ shakeOptions {
        shakeVerbosity = Normal
      , shakeThreads = nc
      , shakeReport = Just (buildDir </> "report.html")
      }

data Options = Options {
    _help :: Bool
  , _verbosity :: Verbosity
  , _jobs :: Int
  , _output :: FilePath
  , _report :: Bool
  , _configuration :: String
  , _targets :: [String]
  } deriving (Show)

defaultOptions :: String -> Options
defaultOptions defaultConfig = Options {
    _help = False
  , _verbosity = Quiet
  , _jobs = 1
  , _output = "./build"
  , _report = False
  , _configuration = defaultConfig
  , _targets = []
  }

makeLenses ''Options
 
arguments :: [String] -> [String] -> Mode Options
arguments cs ts =
    mode "shake" (defaultOptions "debug") "Shake build system"
         (flagArg (updList ts targets) "TARGET..") $
         [ flagHelpSimple (set help True)
         , flagReq ["verbosity","v"] (upd verbosity . read) "VERBOSITY" "Verbosity"
         , flagOpt "1" ["jobs","j"] (upd jobs . read) "NUMBER" "Number of parallel jobs"
         , flagReq ["output", "o"] (upd output) "DIRECTORY" "Build products output directory"
         , flagBool ["report", "r"] (set report) "Generate build report"
         , flagReq ["config", "c"] (updEnum cs configuration) "CONFIGURATION" "Configuration"
         ]
          -- ++ flagsVerbosity (set verbosity)
    where
        upd what x = Right . set what x
        updList xs what x = if x `elem` xs
                            then Right . over what (++[x])
                            else const $ Left $ show x ++ " not in " ++ show xs
        updEnum xs what x = if x `elem` xs
                            then Right . set what x
                            else const $ Left $ show x ++ " not in " ++ show xs

optionsToShake :: Options -> ShakeOptions
optionsToShake opts = shakeOptions {
    shakeThreads = jobs ^$ opts
  , shakeVerbosity = verbosity ^$ opts
  , shakeReport = if report ^$ opts
                    then Just $ (output ^$ opts) </> "shake.html"
                    else Nothing
  }

-- ====================================================================
-- Commandline targets

iOS_SDK = "6.1"

maybeRemoveDirectoryRecursive d =
	Dir.doesDirectoryExist d >>= flip when (Dir.removeDirectoryRecursive d)

targetSpecs :: [(String, (Rules () -> IO ()) -> Env -> IO ())]
targetSpecs = [
    ( "clean", const (maybeRemoveDirectoryRecursive . flip (^.) buildPrefix) )
  , ( "ios",
    \shake env -> do
        developer <- getDeveloperPath
        let target = mkCTarget IOS "armv7"
            toolChain = cToolChain_IOS developer
            buildFlags = applyBuildConfiguration env configurations
                       . methclaStaticBuidFlags
                       . methclaCommonBuildFlags
                       $ cBuildFlags_IOS developer iOS_SDK
        libmethcla <- methclaLib target
        shake $ do
            let libs = [ libmethcla ]
                lib = staticLibrary env target toolChain buildFlags
                libFile = libBuildPath env target
            want =<< mapM lib libs
    )
  , ( "ios-simulator",
    \shake env -> do
        developer <- getDeveloperPath
        let target = mkCTarget IOS_Simulator "i386"
            toolChain = cToolChain_IOS_Simulator developer
            buildFlags = applyBuildConfiguration env configurations
                       . methclaStaticBuidFlags
                       . methclaCommonBuildFlags
                       $ cBuildFlags_IOS_Simulator developer iOS_SDK
        libmethcla <- methclaLib target
        shake $ do
            let libs = [ libmethcla ]
                lib = staticLibrary env target toolChain buildFlags
                libFile = libBuildPath env target
            want =<< mapM lib libs
    )
  , ( "macosx",
    \shake env -> do
        developer <- getDeveloperPath
        sdkVersion <- getSystemVersion
        jackBuildFlags <- pkgConfig "jack"
        let target = mkCTarget MacOSX "x86_64"
            toolChain = cToolChain_MacOSX developer
            buildFlags = applyBuildConfiguration env configurations
                       . jackBuildFlags
                       . methclaSharedBuildFlags
                       . methclaCommonBuildFlags
                       $ cBuildFlags_MacOSX developer sdkVersion
        libmethcla <- methclaLib target
        shake $ do
            let libs = [ libmethcla ]
                lib = sharedLibrary env target toolChain buildFlags
                libFile = libBuildPath env target
            want =<< mapM lib libs
    )
  , ( "tags",
    \shake env -> do
        let and a b = do { as <- a; bs <- b; return $! as ++ bs }
            files clause dir = find always clause dir
            sourceFiles = files (extension ~~? ".h*" ||? extension ~~? ".c*")
            tagFile = "../tags"
            tagFiles = "../tagfiles"
        shake $ do
            tagFile ?=> \output -> do
                fs <- liftIO $
                    find (fileName /=? "typeof") (extension ==? ".hpp") (boostDir </> "boost")
                        `and`
                    files (extension ==? ".hpp") (externalLibraries </> "boost_lockfree")
                        `and`
                    files (extension ==? ".h") (lilvDir </> "lilv")
                        `and`
                    files (extension ==? ".h") (lv2Dir </> "lv2")
                        `and`
                    files (extension ==? ".h") (serdDir </> "serd")
                        `and`
                    files (extension ==? ".h") (sordDir </> "sord")
                        `and`
                    sourceFiles "lv2" `and` sourceFiles "platform" `and` sourceFiles "src"
                need fs
                writeFileLines tagFiles fs
                systemLoud "ctags" $
                    (words "--sort=foldcase --c++-kinds=+p --fields=+iaS --extra=+q --tag-relative=yes")
                 ++ flag_ "-f" output
                 ++ flag_ "-L" tagFiles
                -- FIXME: How to use bracket in the Action monad?
                liftIO $ Dir.removeFile tagFiles
            want [ tagFile ]
    )
  ]

processTargets :: (Rules () -> IO ()) -> Env -> [String] -> IO ()
processTargets shake env = mapM_ processTarget where
    processTarget t =
        case lookup t targetSpecs of
            Nothing -> putStrLn $ "Warning: Target " ++ t ++ " not found"
            Just f -> f shake env

setLineBuffering :: IO ()
setLineBuffering = do
    hSetBuffering stdout LineBuffering
    hSetBuffering stderr LineBuffering

main :: IO ()
main = do
    let args = arguments (map fst configurations) (map fst targetSpecs)
    opts <- processArgs args
    let shakeIt = shake (optionsToShake opts)
        env = mkEnv (configuration ^$ opts) (output ^$ opts)
    if help ^$ opts
        then print $ helpText [] HelpFormatDefault args
        else case targets ^$ opts of
                [] -> do
                    -- TODO: integrate this with option processing
                    putStrLn $ "Please chose a target:"
                    mapM_ (putStrLn . ("\t"++) . fst) targetSpecs
                ts -> setLineBuffering >> processTargets shakeIt env ts
