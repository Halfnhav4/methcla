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

module Shakefile.C where

import           Control.Applicative ((<$>))
import           Control.Lens hiding (Action, (<.>))
import           Control.Monad
import           Data.Monoid
import           Development.Shake as Shake
import           Development.Shake.FilePath
import           Data.Char (toLower)
import           Data.List (intercalate, isSuffixOf)
import           Data.List.Split (splitOn)
import           Data.Maybe
import           GHC.Conc (numCapabilities)
import           System.Console.CmdArgs.Explicit
import           System.Process (readProcess)

import Debug.Trace

{-# DEPRECATE ^$ #-}
(^$) :: forall a c t b. Getting c a t c b -> a -> c
(^$) = flip (^.)

under :: FilePath -> [FilePath] -> [FilePath]
under dir = map prependDir
    where prependDir ""   = dir
          prependDir "."  = dir
          prependDir ".." = takeDirectory dir
          prependDir x    = combine dir x

flag_ :: String -> String -> [String]
flag_ o x = [o, x]

flag :: String -> [String]
flag f = [f]

flags_ :: String -> [String] -> [String]
flags_ o = concat . map (flag_ o)

flags :: String -> [String] -> [String]
flags f = map (f++)

-- Lens utils
append :: forall s t a.
        Monoid a =>
        Setting (->) s t a a -> a -> s -> t
append l n = over l (`mappend` n)

prepend :: forall s t a.
         Monoid a =>
         Setting (->) s t a a -> a -> s -> t
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
defaultArchiver _ toolChain buildFlags inputs output = do
    need inputs
    systemLoud (tool archiverCmd toolChain)
        $ buildFlags ^. archiverFlags
        ++ [output]
        ++ inputs

defaultLinker :: Linker
defaultLinker _ toolChain buildFlags inputs output = do
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
    (stdout, _) <- systemOutput "sed" ["-e", command, input]
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
osxArchiver _ toolChain buildFlags inputs output = do
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
-- Configurations

type Configuration = (String, CBuildFlags -> CBuildFlags)

applyConfiguration :: String -> [Configuration] -> CBuildFlags -> CBuildFlags
applyConfiguration c cs =
    case lookup c cs of
        Nothing -> id
        Just f  -> f

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
