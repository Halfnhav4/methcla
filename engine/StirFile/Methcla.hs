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

{-# LANGUAGE TypeOperators #-}

module Methcla (
    Options
  , buildConfig
  , defaultOptions
  , optionDescrs
  , version
  , Config(..)
  , commonRules
  , mkRules
) where

import           Control.Arrow ((>>>), second)
import           Control.Exception as E
import           Data.Char (toLower)
import           Data.Version (Version(..), showVersion)
import           Development.Shake as Shake
import           Development.Shake.FilePath
import qualified MethclaPro as Pro
import qualified Paths_methcla_stirfile as Package
import           Shakefile.C
import qualified Shakefile.C.Android as Android
import qualified Shakefile.C.Host as Host
import qualified Shakefile.C.NaCl as NaCl
import qualified Shakefile.C.OSX as OSX
import           Shakefile.C.PkgConfig (pkgConfig)
import           Shakefile.Configuration
import           Shakefile.Label
import           Shakefile.SourceTree (SourceTree)
import qualified Shakefile.SourceTree as SourceTree
import           System.Console.GetOpt
import           System.Directory (removeFile)
import qualified System.Environment as Env
import           System.FilePath.Find

{-import Debug.Trace-}

lookupEnv :: String -> IO (Maybe String)
lookupEnv name = do
  e <- E.try $ Env.getEnv name :: IO (Either E.SomeException String)
  case e of
    Left _ -> return Nothing
    Right value -> return $ Just value

-- ====================================================================
-- Library

externalLibraries :: FilePath
externalLibraries = "external_libraries"

externalLibrary :: FilePath -> FilePath
externalLibrary = combine externalLibraries

boostDir :: FilePath
boostDir = externalLibrary "boost"

-- boostBuildFlags :: BuildFlags -> BuildFlags
-- boostBuildFlags = append systemIncludes [ boostDir ]

tlsfDir :: FilePath
tlsfDir = externalLibrary "tlsf"

-- | Build flags common to all targets
commonBuildFlags :: BuildFlags -> BuildFlags
commonBuildFlags = append compilerFlags [
    (Just C, ["-std=c99"])
  , (Just Cpp, ["-std=c++11"])
  , (Nothing, ["-Wall", "-Wextra"])
  , (Just Cpp, ["-fvisibility-inlines-hidden"])
  , (Nothing, ["-Werror=return-type"])
  , (Nothing, ["-fstrict-aliasing", "-Wstrict-aliasing"])
  ]

apiIncludes :: BuildFlags -> BuildFlags
apiIncludes = append systemIncludes [
    "include"
  , "external_libraries/oscpp/include" ]

engineBuildFlags :: FilePath -> BuildFlags -> BuildFlags
engineBuildFlags buildDir =
    append userIncludes
      ( -- Generated headers
        [ buildDir </> "include" ]
        -- Library headers
     ++ [ "src" ]
        -- External libraries
     ++ [ externalLibraries ] )
  . append systemIncludes
       ( -- API headers
         [ "include" ]
         -- Boost
      ++ [ boostDir ]
         -- oscpp
      ++ [ externalLibrary "oscpp/include" ]
         -- TLSF
      ++ [ tlsfDir ] )

pluginBuildFlags :: BuildFlags -> BuildFlags
pluginBuildFlags =
    append systemIncludes [ "include", externalLibrary "oscpp/include" ]
  . append compilerFlags [(Nothing, ["-O3"])]

pluginSources :: SourceTree BuildFlags
pluginSources = SourceTree.files [
    "plugins/node-control.cpp"
  , "plugins/patch-cable.cpp"
  , "plugins/sampler.cpp"
  , "plugins/sine.c"
  , "plugins/soundfile_api_dummy.cpp"
  ]

-- vectorBuildFlags :: BuildFlags -> BuildFlags
-- vectorBuildFlags = append compilerFlags [
--     (Nothing, [ "-O3", "-save-temps" ])
--   ]

methclaSources :: FilePath -> Target -> SourceTree BuildFlags -> SourceTree BuildFlags
methclaSources buildDir target platformSources =
    SourceTree.list [
        -- sourceFlags boostBuildFlags [ SourceTree.files $
        --   under (boostDir </> "libs") [
        --     --   "date_time/src/gregorian/date_generators.cpp"
        --     -- , "date_time/src/gregorian/greg_month.cpp"
        --     -- , "date_time/src/gregorian/greg_weekday.cpp"
        --     -- , "date_time/src/gregorian/gregorian_types.cpp"
        --     -- , "date_time/src/posix_time/posix_time_types.cpp"
        --     -- , "exception/src/clone_current_exception_non_intrusive.cpp"
        --     -- "system/src/error_code.cpp"
        --     ]
        -- ]
        -- TLSF
        SourceTree.files [ tlsfDir </> "tlsf.c" ]
        -- engine
      , SourceTree.flags (engineBuildFlags buildDir) $
          SourceTree.list [
              -- sourceFlags (append compilerFlags [ (Nothing, [ "-O0" ]) ])
              --   [ SourceTree.files [ "src/Methcla/Audio/Engine.cpp" ] ],
              SourceTree.files
                [ "src/Methcla/Audio/AudioBus.cpp"
                , "src/Methcla/Audio/Engine.cpp"
                , "src/Methcla/Audio/Group.cpp"
                , "src/Methcla/Audio/IO/Driver.cpp"
                , "src/Methcla/Audio/Node.cpp"
                -- , "src/Methcla/Audio/Resource.cpp"
                , "src/Methcla/Audio/Synth.cpp"
                , "src/Methcla/Audio/SynthDef.cpp"
                , "src/Methcla/Memory/Manager.cpp"
                , "src/Methcla/Memory.cpp"
                -- Disable for now
                -- , "src/Methcla/Plugin/Loader.cpp"
                , "src/Methcla/Utility/Semaphore.cpp"
                ]
            , SourceTree.filesWithDeps
                [ ("src/Methcla/API.cpp", [ fst (versionHeader buildDir) ]) ]
                -- ++ [ "external_libraries/zix/ring.c" ] -- Unused ATM
              -- platform dependent
            , platformSources
            , Pro.engineSources target
          -- , sourceTree_ (vectorBuildFlags . engineBuildFlags) $ sourceFiles $
          --     under "src" [ "Methcla/Audio/DSP.c" ]
        ]
      , SourceTree.flags pluginBuildFlags $ SourceTree.list $
          [pluginSources] ++ Pro.pluginSources target
      ]

methcla :: String
methcla = "methcla"

version :: String
version = showVersion $ Package.version {
    versionTags = versionTags Package.version ++ Pro.versionTags
  }

versionHeader :: FilePath -> (FilePath, String)
versionHeader buildDir = (buildDir </> "include/Methcla/Version.h", unlines [
    "#ifndef METHCLA_VERSION_H_INCLUDED"
  , "#define METHCLA_VERSION_H_INCLUDED"
  , ""
  , "static const char* kMethclaVersion = \"" ++ version ++ "\";"
  , ""
  , "#endif /* METHCLA_VERSION_H_INCLUDED */"
  ])

-- plugins :: Platform -> [Library]
-- plugins platform = [
--     Library "sine" $ sourceFlags engineBuildFlags [ SourceTree.files [ "plugins/methc.la/plugins/sine/sine.c" ] ]
--   ]

-- ====================================================================
-- Configurations

data Config = Debug | Release deriving (Eq, Show)

parseConfig :: String -> Either String Config
parseConfig x =
    case map toLower x of
        "debug" -> Right Debug
        "release" -> Right Release
        _ -> Left $ "Invalid configuration `" ++ x ++ "'"

configurations :: [Configuration Config BuildFlags]
configurations = [
    ( Release,
        append compilerFlags [
          (Nothing, ["-Os", "-gdwarf-2"])
        , (Nothing, ["-fvisibility=hidden"])
        ]
      . append defines [("NDEBUG", Nothing)]
    )
  , ( Debug,
        append compilerFlags [
            (Nothing, ["-O0", "-gdwarf-2"])
          , (Nothing, ["-fstack-protector", "-Wstack-protector"])
          ]
      . append defines [("DEBUG", Just "1")]
    )
  ]

-- ====================================================================
-- Commandline targets

data Options = Options {
    _buildConfig :: Config
  } deriving (Show)

buildConfig :: Options :-> Config
buildConfig = lens _buildConfig
                   (\g f -> f { _buildConfig = g (_buildConfig f) })

defaultOptions :: Options
defaultOptions = Options {
    _buildConfig = Debug
  }

optionDescrs :: [OptDescr (Either String (Options -> Options))]
optionDescrs = [ Option "c" ["config"]
                   (ReqArg (fmap (set buildConfig) . parseConfig) "CONFIG")
                   "Build configuration (debug, release)." ]

enable :: Bool -> String -> String -> String
enable on flag name = flag ++ (if on then "" else "no-") ++ name

-- | Standard math library.
libm :: BuildFlags -> BuildFlags
libm = append libraries ["m"]

-- | Pthread library.
libpthread :: BuildFlags -> BuildFlags
libpthread = append libraries ["pthread"]

-- | Pass -stdlib=libc++ (clang).
stdlib_libcpp :: ToolChain -> BuildFlags -> BuildFlags
stdlib_libcpp toolChain = onlyIf (get variant toolChain == LLVM) $
    append compilerFlags [(Just Cpp, flags)]
  . append linkerFlags flags
  where flags = ["-stdlib=libc++"]

rtti :: Bool -> BuildFlags -> BuildFlags
rtti on = append compilerFlags [(Just Cpp, [enable on "-f" "rtti"])]

exceptions :: Bool -> BuildFlags -> BuildFlags
exceptions on = append compilerFlags [(Just Cpp, [enable on "-f" "exceptions"])]

execStack :: Bool -> BuildFlags -> BuildFlags
execStack on =
    append compilerFlags [(Nothing, ["-Wa,--" ++ no ++ "execstack"])]
  . append linkerFlags ["-Wl,-z," ++ no ++ "execstack"]
  where no = if on then "" else "no"

withTarget :: Monad m => (Arch -> Target) -> [Arch] -> (Target -> m ()) -> m ()
withTarget mkTarget archs f = mapM_ (f . mkTarget) archs

mapTarget :: Monad m => (Arch -> Target) -> [Arch] -> (Target -> m a) -> m [a]
mapTarget mkTarget archs f = mapM (f . mkTarget) archs

androidTargetPlatform :: Platform
androidTargetPlatform = Android.platform 9

mkBuildPrefix :: Show a => FilePath -> a -> FilePath -> FilePath
mkBuildPrefix buildDir config target = buildDir </> map toLower (show config) </> target

commonRules :: FilePath -> Rules ()
commonRules buildDir = do
  let header = versionHeader buildDir
  fst header *> \file -> do
    alwaysRerun
    writeFileChanged file (snd header)
  phony "clean" $ removeFilesAfter buildDir ["//*"]

copyTo :: FilePath -> FilePath -> Rules FilePath
copyTo input output = do
  output ?=> \_ -> copyFile' input output
  return output

phonyFiles :: String -> [FilePath] -> Rules ()
phonyFiles target = phony target . need

phonyFile :: String -> FilePath -> Rules ()
phonyFile target input = phonyFiles target [input]

mkRules :: FilePath -> Options -> [([String], IO (Rules ()))]
mkRules buildDir options = do
    let config = get buildConfig options
        mkEnv target = set buildPrefix
                            (defaultBuildPrefix buildDir target (show config))
                            -- (mkBuildPrefix config (platformString $ get targetPlatform target))
                            defaultEnv
        platformAlias p = phony (platformString p) . need . (:[])
        targetAlias target = phony (platformString (get targetPlatform target) ++ "-" ++ archString (get targetArch target))
                                . need . (:[])
    [ (["iphoneos", "iphonesimulator", "iphone-universal"], do
        let iOS_SDK = Version [6,1] []
            iosBuildFlags toolChain =
                    OSX.iphoneos_version_min (Version [5,0] [])
                >>> append userIncludes [ "platform/ios"
                                        , externalLibrary "CoreAudioUtilityClasses/CoreAudio/PublicUtility" ]
                >>> stdlib_libcpp toolChain
            iosSources target = methclaSources buildDir target $ SourceTree.files $
                                [ "platform/ios/Methcla/Audio/IO/RemoteIODriver.cpp"
                                , externalLibrary "CoreAudioUtilityClasses/CoreAudio/PublicUtility/CAHostTimeBase.cpp" ]
                                ++ if Pro.isPresent then [ "platform/ios/Methcla/ProAPI.cpp" ] else []

        applyEnv <- toolChainFromEnvironment
        developer <- OSX.getDeveloperPath
        return $ do
            iphoneosLibs <- mapTarget (flip OSX.target (OSX.iPhoneOS iOS_SDK)) [Arm Armv7, Arm Armv7s] $ \target -> do
                let toolChain = applyEnv $ OSX.toolChain developer target
                    env = mkEnv target
                    buildFlags =   applyConfiguration config configurations
                               >>> commonBuildFlags
                               >>> iosBuildFlags toolChain
                lib <- staticLibrary env target toolChain methcla (SourceTree.flags buildFlags (iosSources target))
                return lib
            iphoneosLib <- OSX.universalBinary
                            iphoneosLibs
                            (mkBuildPrefix buildDir config "iphoneos" </> "libmethcla.a")
            phony "iphoneos" (need [iphoneosLib])

            iphonesimulatorLibI386 <- do
                let platform = OSX.iPhoneSimulator iOS_SDK
                    target = OSX.target (X86 I386) platform
                    toolChain = applyEnv $ OSX.toolChain developer target
                    env = mkEnv target
                    buildFlags =   applyConfiguration config configurations
                               >>> commonBuildFlags
                               >>> iosBuildFlags toolChain
                lib <- staticLibrary env target toolChain methcla (SourceTree.flags buildFlags (iosSources target))
                return lib
            let iphonesimulatorLib = mkBuildPrefix buildDir config "iphonesimulator" </> "libmethcla.a"
            iphonesimulatorLib *> copyFile' iphonesimulatorLibI386
            phony "iphonesimulator" (need [iphonesimulatorLib])

            let universalTarget = "iphone-universal"
            universalLib <- OSX.universalBinary
                                (iphoneosLibs ++ [iphonesimulatorLib])
                                (mkBuildPrefix buildDir config universalTarget </> "libmethcla.a")
            phony universalTarget (need [universalLib])
        )
      , (["android", "android-tests"], do
            applyEnv <- toolChainFromEnvironment
            ndk <- Env.getEnv "ANDROID_NDK"
            return $ do
                libs <- mapTarget (flip Android.target androidTargetPlatform) [Arm Armv5, Arm Armv7] $ \target -> do
                    let abi = Android.abiString (get targetArch target)
                        toolChain = applyEnv $ Android.toolChain ndk GCC (Version [4,7] []) target
                        buildFlags =   applyConfiguration config configurations
                                   >>> commonBuildFlags
                                   >>> append userIncludes ["platform/android"]
                                   >>> Android.gnustl Static ndk target
                                   >>> rtti True
                                   >>> exceptions True
                                   >>> execStack False
                    libmethcla <- staticLibrary (mkEnv target) target toolChain methcla $
                                    SourceTree.flags buildFlags $ methclaSources buildDir target $
                                        SourceTree.files [
                                          "platform/android/opensl_io.c",
                                          "platform/android/Methcla/Audio/IO/OpenSLESDriver.cpp" ]
                    let testBuildFlags =
                                       engineBuildFlags buildDir
                                   >>> append systemIncludes [externalLibrary "catch/single_include"]
                                   >>> append linkerFlags ["-Wl,-soname,libmethcla-tests.so"]
                                   >>> append localLibraries [libmethcla]
                                   >>> append libraries ["android", "log", "OpenSLES"]
                                   >>> buildFlags
                    libmethcla_tests <- sharedLibrary (mkEnv target) target toolChain
                                  "methcla-tests"
                                  -- TODO: Build static library for native_app_glue to link against.
                                  $ SourceTree.flags testBuildFlags
                                  $ SourceTree.append (Android.native_app_glue ndk)
                                                      (SourceTree.files [
                                                          "src/Methcla/Audio/IO/DummyDriver.cpp"
                                                        , "tests/methcla_tests.cpp"
                                                        , "tests/android/main.cpp" ])

                    let installPath = "libs/android" </> abi </> takeFileName libmethcla
                    installPath ?=> \_ -> copyFile' libmethcla installPath
                    targetAlias target installPath

                    let testInstallPath = "tests/android/libs" </> Android.abiString (get targetArch target) </> takeFileName libmethcla_tests
                    testInstallPath ?=> \_ -> copyFile' libmethcla_tests testInstallPath
                    return (installPath, testInstallPath)
                phony "android" $ need $ map fst libs
                phony "android-tests" $ need $ map snd libs
        )
      , (["pnacl", "pnacl-test", "pnacl-examples"], do
        sdk <- Env.getEnv "NACL_SDK"
        libsndfile <- pkgConfig "sndfile"
        freesoundApiKey <- Env.getEnv "FREESOUND_API_KEY"
        return $ do
          let -- target = NaCl.target (NaCl.pepper 31)
              target = NaCl.target NaCl.canary
              naclConfig = case config of
                            Debug -> NaCl.Debug
                            Release -> NaCl.Release
              toolChain = NaCl.toolChain
                            sdk
                            naclConfig
                            target
              env = mkEnv target
              buildFlags =   applyConfiguration config configurations
                         >>> commonBuildFlags
                         -- Currently -std=c++11 produces compile errors with libc++
                         -- -std=c++11 defines __STRICT_ANSI__ and then newlib doesn't export fileno (needed by catch)
                         >>> append compilerFlags [(Just Cpp, ["-std=gnu++11"])]
                         -- Not detected by boost for PNaCl platform
                         -- >>> append defines [("BOOST_HAS_PTHREADS", Nothing), ("METHCLA_USE_BOOST_THREAD", Just "1")]
                         >>> append userIncludes ["platform/pepper"]
                         >>> stdlib_libcpp toolChain
                         >>> append linkerFlags ["--pnacl-exceptions=sjlj"]
                         >>> NaCl.libppapi_cpp
                         >>> NaCl.libppapi
                         >>> libpthread
          libmethcla <- staticLibrary env target toolChain methcla $
                              SourceTree.flags buildFlags $ methclaSources buildDir target $
                                SourceTree.files [
                                    "platform/pepper/Methcla/Audio/IO/PepperDriver.cpp" ]
          phony "pnacl" $ need [libmethcla]
          let testBuildFlags =   buildFlags
                             >>> append defines [ ("METHCLA_TEST_SOUNDFILE_API_HEADER", Just "<methcla/plugins/soundfile_api_dummy.h>")
                                                , ("METHCLA_TEST_SOUNDFILE_API_LIB", Just "methcla_soundfile_api_dummy") ]
                             >>> append systemIncludes [externalLibrary "catch/single_include"]
                             -- >>> NaCl.libppapi_simple
                             -- >>> NaCl.libnacl_io
                             >>> Pro.testBuildFlags target
          pnacl_test_bc <- executable env target toolChain "methcla-pnacl-tests"
                            $ SourceTree.flags testBuildFlags
                            $ methclaSources buildDir target $ SourceTree.list [
                                SourceTree.files [
                                    "src/Methcla/Audio/IO/DummyDriver.cpp"
                                  , "tests/methcla_tests.cpp"
                                  , "tests/methcla_engine_tests.cpp"
                                  , "tests/test_runner_nacl.cpp"
                                  ]
                              , Pro.testSources target ]
          pnacl_test <- NaCl.finalize toolChain pnacl_test_bc (pnacl_test_bc `replaceExtension` "pexe")
          pnacl_test_nmf <- NaCl.mk_nmf [(NaCl.PNaCl, pnacl_test)]
                                        (pnacl_test `replaceExtension` "nmf")
          pnacl_test' <- pnacl_test `copyTo` ("tests/pnacl" </> takeFileName pnacl_test)
          pnacl_test_nmf' <- pnacl_test_nmf `copyTo` ("tests/pnacl" </> takeFileName pnacl_test_nmf)
          phonyFiles "pnacl-test" [pnacl_test', pnacl_test_nmf']

          let examplesBuildFlags =   buildFlags
                                 >>> append systemIncludes ["include"]
                                 >>> append localLibraries [libmethcla]
              examplesDir = buildDir </> "examples/pnacl"
              mkExample output flags sources files = do
                let outputDir = takeDirectory output
                bc <- executable env target toolChain (takeFileName output)
                        $ SourceTree.flags
                          (examplesBuildFlags >>> flags)
                        $ SourceTree.files sources
                pexe <- NaCl.finalize
                          toolChain
                          bc
                          (bc `replaceExtension` "pexe"
                              `replaceDirectory` (outputDir </> show naclConfig))
                nmf <- NaCl.mk_nmf
                          [(NaCl.PNaCl, pexe)]
                          (pexe `replaceExtension` "nmf")
                files' <- mapM (\old -> old `copyTo` (old `replaceDirectory` outputDir))
                               (["examples/common/common.js"] ++ files)
                return $ [pexe, nmf] ++ files'

          thaddeus <- mkExample ( examplesDir </> "thaddeus/methcla-thaddeus" )
                                ( append userIncludes [ "examples/thADDeus/src" ] )
                                [ "examples/thADDeus/pnacl/main.cpp"
                                , "examples/thADDeus/src/synth.cpp"
                                ]
                                [ "examples/thADDeus/pnacl/index.html"
                                , "examples/thADDeus/pnacl/manifest.json"
                                , "examples/thADDeus/pnacl/thaddeus.js"
                                ]
          sampler <- mkExample  ( examplesDir </> "sampler/methcla-sampler" )
                                ( append userIncludes   [ "examples/sampler/src" ]
                                . append systemIncludes [ "examples/sampler/libs/tinydir" ]
                                . libsndfile
                                . NaCl.libnacl_io )
                                [ "examples/sampler/pnacl/main.cpp"
                                , "examples/sampler/src/Engine.cpp"
                                , "plugins/soundfile_api_libsndfile.cpp"
                                ]
                                [ "examples/sampler/pnacl/index.html"
                                , "examples/sampler/pnacl/manifest.json"
                                , "examples/sampler/pnacl/example.js"
                                ]

          let freesounds = map (takeFileName.takeDirectory) [
                  "http://freesound.org/people/adejabor/sounds/157965/"
                , "http://freesound.org/people/NOISE.INC/sounds/45394/"
                , "http://freesound.org/people/ThePriest909/sounds/209331/"
                ]

          (examplesDir </> "sampler/sounds/*") *> \output -> do
            cmd "curl" [    "http://www.freesound.org/api/sounds/"
                         ++ dropExtension (takeFileName output)
                         ++ "/serve?api_key=" ++ freesoundApiKey
                       , "-L", "-o", output ] :: Action ()

          -- Install warp-static web server if needed
          let server = ".cabal-sandbox/bin/warp"

          server *> \_ -> do
            cmd "cabal sandbox init" :: Action ()
            cmd "cabal install -j warp-static" :: Action ()

          phony "pnacl-examples" $ do
            need [server]
            need thaddeus
            need $ sampler ++ map (combine (examplesDir </> "sampler/sounds"))
                                  freesounds
            cmd (Cwd examplesDir) server :: Action ()
        )
      , (["icecast", "icecast-example"], do -- macosx-icecast
            applyEnv <- toolChainFromEnvironment
            (target, toolChain) <- fmap (second applyEnv) Host.getDefaultToolChain
            libshout <- pkgConfig "shout"
            let env = mkEnv target
                liblame = append systemIncludes ["/usr/local/include"]
                        . append libraryPath ["/usr/local/lib"]
                        . append libraries ["mp3lame"]
                buildFlags =   applyConfiguration config configurations
                           >>> commonBuildFlags
                           >>> append userIncludes ["platform/icecast"]
                           >>> libshout
                           >>> liblame
                           >>> stdlib_libcpp toolChain
            return $ do
                lib <- staticLibrary env target toolChain
                            "methcla-icecast"
                          $ SourceTree.flags buildFlags
                          $ methclaSources buildDir target
                          $ SourceTree.files ["platform/icecast/Methcla/Audio/IO/IcecastDriver.cpp"]
                example <- executable env target toolChain
                            "methcla-icecast-example"
                            $ SourceTree.flags (    apiIncludes
                                                >>> append userIncludes ["examples/thADDeus/src"]
                                                >>> append localLibraries [lib]
                                                >>> buildFlags )
                            $ SourceTree.files [ "examples/icecast/main.cpp"
                                               , "examples/thADDeus/src/synth.cpp" ]
                phony "icecast" $ need [lib]
                phony "icecast-example" $ need [example]
            )
      , (["jack"], do -- macosx-jack
            applyEnv <- toolChainFromEnvironment
            (target, toolChain) <- fmap (second applyEnv) Host.getDefaultToolChain
            jackBuildFlags <- pkgConfig "jack"
            libsndfile <- pkgConfig "sndfile"
            let env = mkEnv target
                buildFlags =   applyConfiguration config configurations
                           >>> commonBuildFlags
                           >>> append userIncludes ["platform/jack"]
                           >>> jackBuildFlags
                           >>> libsndfile
                           >>> stdlib_libcpp toolChain
                           >>> libm
                build f = f env target toolChain "methcla-jack"
                            $ SourceTree.flags buildFlags
                            $ methclaSources buildDir target $
                                SourceTree.files [ "platform/jack/Methcla/Audio/IO/JackDriver.cpp"
                                                 , "plugins/soundfile_api_libsndfile.cpp" ]
            return $ do
                staticLib <- build staticLibrary
                sharedLib <- build sharedLibrary
                phony "jack" $ need [staticLib, sharedLib]
        )
      , (["test", "clean-test"], do -- tests
            applyEnv <- toolChainFromEnvironment
            (target, toolChain) <- fmap (second applyEnv) Host.getDefaultToolChain
            let env = mkEnv target
                buildFlags =   applyConfiguration config configurations
                           >>> commonBuildFlags
                           >>> append systemIncludes [externalLibrary "catch/single_include"]
                           >>> stdlib_libcpp toolChain
                           >>> Host.onlyOn [Host.Linux] libpthread
                           >>> libm
                           >>> Pro.testBuildFlags target
            return $ do
                result <- executable env target toolChain "methcla-tests"
                            $ SourceTree.flags buildFlags
                            $ methclaSources buildDir target $ SourceTree.list [
                                SourceTree.files [
                                    "src/Methcla/Audio/IO/DummyDriver.cpp"
                                  , "tests/methcla_tests.cpp"
                                  , "tests/methcla_engine_tests.cpp"
                                  , "tests/test_runner_console.cpp" ]
                              , Pro.testSources target ]
                phony "test" $ do
                    need [result]
                    system' result []
                phony "clean-test" $ removeFilesAfter "tests/output" ["*.osc", "*.wav"]
        )
      , (["tags"], do -- tags
            let and_ a b = do { as <- a; bs <- b; return $! as ++ bs }
                files clause dir = find always clause dir
                sources = files (extension ~~? ".h*" ||? extension ~~? ".c*")
                tagFile = "tags"
                tagFiles = "tagfiles"
            return $ do
                tagFile ?=> \output -> flip actionFinally (removeFile tagFiles) $ do
                    fs <- liftIO $ find
                              (fileName /=? "typeof") (extension ==? ".hpp") (boostDir </> "boost")
                        `and_` sources "include"
                        `and_` sources "platform"
                        `and_` sources "plugins"
                        `and_` sources "src"
                    need fs
                    writeFileLines tagFiles fs
                    system' "ctags" $
                        (words "--sort=foldcase --c++-kinds=+p --fields=+iaS --extra=+q --tag-relative=yes")
                     ++ ["-f", output]
                     ++ ["-L", tagFiles]
        )
        ]
