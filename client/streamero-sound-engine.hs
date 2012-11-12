{-# LANGUAGE ExistentialQuantification #-}
{-# LANGUAGE FlexibleContexts #-}
{-# LANGUAGE GeneralizedNewtypeDeriving #-}
{-# LANGUAGE OverloadedStrings #-}

import           Control.Applicative
import           Control.Arrow (first, second)
import           Control.Category ((.))
import           Control.Concurrent (forkIO, myThreadId, threadDelay)
import qualified Control.Concurrent.Chan as Chan
import qualified Control.Concurrent.MVar as MVar
import qualified Control.Concurrent.STM as STM
import qualified Control.Exception.Lifted as E
import           Control.Failure (Failure, failure)
import           Control.Monad (MonadPlus, foldM, join, mzero, unless, void, when)
import           Control.Monad.IO.Class (MonadIO, liftIO)
import           Control.Monad.Trans.Class (lift)
import           Data.Aeson (FromJSON(..), ToJSON(..), Value(..), (.=), (.:), (.:?), (.!=), object)
import qualified Data.Aeson as J
import qualified Data.Aeson.Types as J
import qualified Data.Attoparsec.ByteString.Char8 as A
import qualified Data.Bits as Bits
import qualified Data.ByteString as BS
import qualified Data.ByteString.Lazy as BL
import qualified Data.ByteString.Lazy.Char8 as BC
import           Data.Conduit (($$), (=$=))
import qualified Data.Conduit as C
import qualified Data.Conduit.Attoparsec as C
import qualified Data.Conduit.Binary as CB
import qualified Data.Conduit.Cereal as C
import qualified Data.Conduit.List as CL
import qualified Data.Conduit.Network as C
import qualified Data.Foldable as F
import qualified Data.Hashable as H
import qualified Data.HashMap.Strict as H
import qualified Data.HashSet as HS
import           Data.Lens.Common
--import           Data.Lens.Template
import qualified Data.List as List
import           Data.Maybe (catMaybes, maybeToList)
import           Data.Serialize.Get (getWord32be)
import           Data.Serialize.Put (putLazyByteString, putWord32be, runPut)
import           Data.Text (Text)
import qualified Data.Text as Text
import qualified Data.Traversable as T
import           Data.Word (Word64)
import qualified Filesystem as FS
import qualified Filesystem.Path.CurrentOS as FS
import qualified Network.Socket as N
import           Network.URL (URL)
import qualified Network.URL as URL
import           Prelude hiding ((.), catch, unlines)
import           Reactive.Banana ((<@>), (<@))
import qualified Reactive.Banana as R
import           Sound.OpenSoundControl (immediately)
import qualified Sound.OpenSoundControl as OSC
import qualified Sound.SC3.UGen as SC
import qualified Sound.SC3.Server.Allocator as SC
import qualified Sound.SC3.Server.Monad as SC
import qualified Sound.SC3.Server.Monad.Command as SC
import qualified Sound.SC3.Server.Monad.Process as SC
import qualified Sound.SC3.Server.Monad.Request as SC
import qualified Sound.File.Sndfile as SF
import qualified Streamero.Darkice as Darkice
import           Streamero.Exception (Exception(..))
import qualified Streamero.Jack as SJack
import qualified Streamero.MonadServer as MonadServer
import qualified Streamero.Process as SProc
{-import           Streamero.Readline (sourceReadline)-}
import           Streamero.SC3 (control, toStereo)
import           Streamero.Signals (ignoreSignals, catchSignals)
import qualified Streamero.SoundFile as SF
import           System.Console.CmdArgs.Explicit
import qualified System.Directory as Dir
import           System.FilePath ((</>))
import           System.IO
import           System.Posix.Signals (Signal, sigINT, sigTERM)
import           System.Process (ProcessHandle)
import           System.Unix.Directory (withTemporaryDirectory)

-- TODO:
-- * Use individual events for field updates
-- * Refactor into modules
-- * Place player synths in one group and patch cables in another (ordered after the first group) to avoid order of execution problems
-- * Only update the patch cables for locations the listener is currently in.
-- * Use playBuf for small sound files (e.g. <= 32768)

-- --------------------------------------------------------------------
-- Conduit helpers

catMaybesC :: Monad m => C.Conduit (Maybe a) m a
catMaybesC = do
    v <- C.await
    case v of
        Nothing -> return ()
        Just Nothing -> catMaybesC
        Just (Just a) -> C.yield a >> catMaybesC

printC :: (Show a, MonadIO m) => C.Conduit a m a
printC = do
    v <- C.await
    case v of
        Nothing -> return ()
        Just a -> liftIO (print a) >> C.yield a >> printC

-- | Chunk input into messages delimited by 32 bit integer byte counts.
message :: C.MonadThrow m => C.Conduit BS.ByteString m BC.ByteString
message = C.sequence $ do
    n <- C.sinkGet getWord32be
    CB.take (fromIntegral n)

-- | Flatten messages into the output stream, prepending them with a 32 bit integer byte count.
unmessage :: Monad m => C.Conduit BC.ByteString m BS.ByteString
unmessage = CL.map $ \b -> runPut $ putWord32be (fromIntegral (BC.length b)) >> putLazyByteString b

-- | Flatten messages into lines in the output stream.
unlines :: Monad m => C.Conduit BC.ByteString m BS.ByteString
unlines = CL.map (BS.concat . BL.toChunks . flip BC.snoc '\n')

-- | Convert JSON values to Haskell values.
fromJson :: (C.MonadThrow m, FromJSON a) => C.Conduit J.Value m a
fromJson = go where
    go = do
        next <- C.await
        case next of
            Nothing -> return ()
            Just v -> case J.fromJSON v of
                        J.Success a -> C.yield a >> go
                        J.Error e   -> lift . C.monadThrow . C.ParseError [] $ e

-- | Convert Haskell values to JSON values.
toJson :: (MonadIO m, ToJSON a) => C.Conduit a m BC.ByteString
toJson = CL.map J.encode

-- | Parse JSON messages into JSON values.
parseJsonMessage :: C.MonadThrow m => C.Conduit BC.ByteString m J.Value
parseJsonMessage = go where
    go = do
        next <- C.await
        case next of
            Nothing -> return ()
            Just b -> case A.parse J.json (BS.concat $ BC.toChunks $ b) of
                        A.Done _ v    -> C.yield v >> go
                        A.Fail _ cs e -> lift . C.monadThrow . C.ParseError cs $ e
                        A.Partial _   -> lift $ C.monadThrow C.DivergentParser

-- | Parse JSON or whitespace.
jsonOrWhite :: A.Parser (Maybe J.Value)
jsonOrWhite = (Just <$> J.json) <|> (A.many1 A.space >> return Nothing)

-- | Conduit wrapping IO functions.
conduitIO :: MonadIO m => (input -> IO ()) -> (IO output) -> C.Conduit input m output
conduitIO inputFunc outputFunc = do
    x <- C.await
    case x of
        Nothing -> return ()
        Just input -> do
            liftIO $ inputFunc input
            output <- liftIO outputFunc
            C.yield output
            conduitIO inputFunc outputFunc

-- | Parse a stream of JSON messages into JSON values.
parseJsonStream :: C.MonadThrow m => C.Conduit BS.ByteString m J.Value
parseJsonStream = C.sequence (C.sinkParser jsonOrWhite) =$= catMaybesC

-- --------------------------------------------------------------------
-- JSON helpers

mkUpdate :: FromJSON x => Text -> (x -> a -> a) -> J.Value -> J.Parser (a -> a)
mkUpdate key set (Object v) = maybe id set <$> v .:? key
mkUpdate _ _ _ = mzero

-- --------------------------------------------------------------------
-- URL helpers

setHostName :: String -> URL -> URL
setHostName h u =
  case URL.url_type u of
    URL.Absolute host -> u { URL.url_type = URL.Absolute (host { URL.host = h }) }
    _ -> u

parseURL :: MonadPlus m => String -> m URL
parseURL a =
    case URL.importURL a of
        Nothing -> mzero
        Just url -> return url

-- --------------------------------------------------------------------
-- Model data types

data Coord = Coord { latitude :: Double, longitude :: Double }
             deriving (Eq, Show)

instance FromJSON Coord where
    parseJSON (Object v) = Coord <$> v .: "latitude" <*> v .: "longitude"
    parseJSON _          = mzero

-- | Add one coordinate as an offset to another.
offset :: Coord -> Coord -> Coord
offset c1 c2 = Coord (latitude c1 + latitude c2) (longitude c1 + longitude c2)

-- | Distance in meters between two coordinates.
distance :: Coord -> Coord -> Double
distance c1 c2 = earthRadius * ahaversin h
    where
        earthRadius = 6371e3
        sqr x = x * x
        haversin = sqr . sin . (/2)
        ahaversin = (*2) . asin . sqrt
        deg2rad = (/180) . (*pi)
        h = haversin (deg2rad (latitude c2) - deg2rad (latitude c1)) +
               cos (deg2rad (latitude c1)) * cos (deg2rad (latitude c2))
                   * haversin (deg2rad (longitude c2) - deg2rad (longitude c1))

newtype SoundId = SoundId Int deriving (Eq, Show, H.Hashable, FromJSON)

data SoundType =
  SoundFile { path :: FilePath }
  deriving (Show)

instance FromJSON SoundType where
  parseJSON (Object v) = do
    t <- v .: "type" :: J.Parser Text
    case t of
      "sample" -> SoundFile <$> v .: "path"
      _ -> mzero
  parseJSON _ = mzero

data Sound = Sound {
    soundId :: SoundId
  , soundType :: SoundType
  , isEvent :: Bool
  } deriving (Show)

isContinuous :: Sound -> Bool
isContinuous = not . isEvent

instance FromJSON Sound where
    parseJSON v@(Object o) =
      Sound
      <$> o .: "id"
      <*> parseJSON v
      <*> o .:? "event" .!= False
    parseJSON _ = mzero

newtype SoundUpdate = SoundUpdate (Sound -> Sound)

instance FromJSON SoundUpdate where
  parseJSON = fmap SoundUpdate . mkUpdate "event" (\x s -> s { isEvent = x })

data SoundFileInfo = SoundFileInfo {
    sampleRate :: Int
  , numChannels :: Int
  , numFrames :: Word64
  } deriving (Show)

newtype ListenerId = ListenerId Text deriving (Eq, Show, H.Hashable, FromJSON)

data Listener = Listener {
    listenerId :: ListenerId
  , listenerPosition :: Coord
  , streamURL :: URL
  } deriving (Show)

instance Eq Listener where
  a == b = listenerId a == listenerId b

instance H.Hashable Listener where
  hash = H.hash . listenerId

instance FromJSON Listener where
  parseJSON (Object v) =
    Listener
    <$> v .: "id"
    <*> v .: "position"
    <*> (parseURL =<< (v .: "stream_url"))
  parseJSON _ = mzero

newtype ListenerUpdate = ListenerUpdate (Listener -> Listener)

instance FromJSON ListenerUpdate where
  parseJSON = fmap ListenerUpdate . mkUpdate "position" (\p l -> l { listenerPosition = p })

data ListenerState = ListenerState {
    outputBus :: SC.AudioBus
  , streamer :: ProcessHandle
  }

instance Show ListenerState where
  show x = "ListenerState " ++ show (outputBus x)

type ListenerMap = H.HashMap ListenerId (Listener, ListenerState)

data Reference = Absolute | Relative deriving (Eq, Show)

newtype LocationId = LocationId Int deriving (Eq, Show, H.Hashable, FromJSON)

data Location = Location {
  locationId :: LocationId
, position :: Coord
, reference :: Reference
, radius :: Double
, locationSounds :: [SoundId]
} deriving (Show)

instance Eq Location where
  a == b = locationId a == locationId b

instance H.Hashable Location where
  hash = H.hash . locationId

instance FromJSON Location where
    parseJSON (Object v) =
      Location
      <$> v .: "id"
      <*> v .: "position"
      <*> ((\b -> if b then Relative else Absolute) <$> v .:? "relative" .!= False)
      <*> v .: "radius"
      <*> v .:? "sounds" .!= []
    parseJSON _ = mzero

newtype LocationUpdate = LocationUpdate (Location -> Location)

instance FromJSON LocationUpdate where
  parseJSON v = LocationUpdate <$>
    foldM (\f -> fmap ((.)f)) id
          [ mkUpdate "position" (\x s -> s { position = x })        v
          , mkUpdate "radius"   (\x s -> s { radius = x })          v
          , mkUpdate "sounds"   (\x s -> s { locationSounds = x })  v ]

locationDistance :: Location -> Coord -> Double
locationDistance location coord =
    case reference location of
        Absolute -> distance coord (position location)
        Relative -> distance coord (coord `offset` position location)

data LocationState = LocationState {
  bus :: SC.AudioBus
, players :: H.HashMap SoundId Player
} deriving (Show)

type LocationMap = H.HashMap LocationId (Location, LocationState)

data Request =
    Quit
  | AddListener Listener
  | RemoveListener ListenerId
  | UpdateListener ListenerId (Listener -> Listener)
  | AddSound Sound
  | UpdateSound SoundId (Sound -> Sound)
  | AddLocation Location
  | RemoveLocation LocationId
  | UpdateLocation LocationId (Location -> Location)

instance FromJSON Request where
  parseJSON v@(Object o) = do
    t <- o .: "request" :: J.Parser Text
    case t of
      "Quit"           -> pure Quit
      "AddListener"    -> AddListener
                          <$> J.parseJSON v
      "RemoveListener" -> RemoveListener
                          <$> o .: "id"
      "UpdateListener" -> UpdateListener
                          <$> o .: "id"
                          <*> fmap (\(ListenerUpdate f) -> f) (J.parseJSON v)
      "AddSound"       -> AddSound
                          <$> J.parseJSON v
      "UpdateSound"    -> UpdateSound
                          <$> o .: "id"
                          <*> fmap (\(SoundUpdate f) -> f) (J.parseJSON v)
      "AddLocation"    -> AddLocation
                          <$> J.parseJSON v
      "RemoveLocation" -> RemoveLocation
                          <$> o .: "id"
      "UpdateLocation" -> UpdateLocation
                          <$> o .: "id"
                          <*> fmap (\(LocationUpdate f) -> f) (J.parseJSON v)
      _ -> mzero
  parseJSON _ = mzero

data Response = Ok | Error Exception

instance ToJSON Response where
    toJSON Ok = object [ "response" .= ("Ok" :: Text) ]
    toJSON (Error e) = object [ "response" .= ("Error" :: Text), "exception" .= e ]

data Options = Options {
    _help :: Bool
  , _socket :: Maybe FilePath
  , _jackPath :: FilePath
  , _jackDriver :: String
  , _maxNumListeners :: Int
  , _scUdpPort :: Int
  , _monitor :: Bool
  , _local :: Bool
  } deriving (Show)

defaultOptions :: Options
defaultOptions = Options {
    _help = False
  , _socket = Nothing
  , _jackPath = "jackd"
  , _jackDriver = "dummy"
  , _maxNumListeners = 1
  , _scUdpPort = 57110
  , _monitor = False
  , _local = False
  }

-- $( makeLenses [''Options] )

help = lens
  _help (\x s -> s {
  _help = x })
socket = lens
  _socket (\x s -> s {
  _socket = x })
jackPath = lens
  _jackPath (\x s -> s {
  _jackPath = x })
jackDriver = lens
  _jackDriver (\x s -> s {
  _jackDriver  = x })
maxNumListeners = lens
  _maxNumListeners (\x s -> s {
  _maxNumListeners = x })
scUdpPort = lens
  _scUdpPort (\x s -> s {
  _scUdpPort = x })
monitor = lens
  _monitor (\x s -> s {
  _monitor = x })
local = lens
  _local (\x s -> s {
  _local = x })

data Environment = Env {
    _options :: Options
  , _temporaryDirectory :: FS.FilePath
  , _jackServerName :: String
  } deriving (Show)

makeEnv :: Options -> FS.FilePath -> String -> Environment
makeEnv = Env

-- $( makeLenses [''Environment] )
options = lens
  _options (\x s -> s {
  _options = x })
temporaryDirectory = lens
  _temporaryDirectory (\x s -> s {
  _temporaryDirectory = x })
jackServerName = lens
  _jackServerName (\x s -> s {
  _jackServerName = x })

data EventSources t = EventSources {
  eAddSound :: R.Event t Sound
, eUpdateSound :: R.Event t (SoundId, Sound -> Sound)
, eAddLocation :: R.Event t Location
, eRemoveLocation :: R.Event t LocationId
, eUpdateLocation :: R.Event t (LocationId, Location -> Location)
, eAddListener :: R.Event t Listener
, eRemoveListener :: R.Event t ListenerId
, eUpdateListener :: R.Event t (ListenerId, Listener -> Listener)
, eQuit :: R.Event t ()
}

data EventSinks = EventSinks {
  fireAddSound :: Sound -> IO ()
, fireUpdateSound :: (SoundId, Sound -> Sound) -> IO ()
, fireAddLocation :: Location -> IO ()
, fireRemoveLocation :: LocationId -> IO ()
, fireUpdateLocation :: (LocationId, Location -> Location) -> IO ()
, fireAddListener :: Listener -> IO ()
, fireRemoveListener :: ListenerId -> IO ()
, fireUpdateListener :: (ListenerId, Listener -> Listener) -> IO ()
, fireQuit :: () -> IO ()
}

mkInputEvents = do
  (eAddSound, fAddSound) <- R.newEvent
  (eUpdateSound, fUpdateSound) <- R.newEvent
  (eAddLocation, fAddLocation) <- R.newEvent
  (eRemoveLocation, fRemoveLocation) <- R.newEvent
  (eUpdateLocation, fUpdateLocation) <- R.newEvent
  (eAddListener, fAddListener) <- R.newEvent
  (eRemoveListener, fRemoveListener) <- R.newEvent
  (eUpdateListener, fUpdateListener) <- R.newEvent
  (eQuit, fQuit) <- R.newEvent
  return
    (
      EventSources
        eAddSound
        eUpdateSound
        eAddLocation
        eRemoveLocation
        eUpdateLocation
        eAddListener
        eRemoveListener
        eUpdateListener
        eQuit
    , EventSinks
        fAddSound
        fUpdateSound
        fAddLocation
        fRemoveLocation
        fUpdateLocation
        fAddListener
        fRemoveListener
        fUpdateListener
        fQuit
    )

requestToEvent :: EventSinks -> Request -> IO ()
requestToEvent es (AddSound x) = fireAddSound es x
requestToEvent es (UpdateSound i x) = fireUpdateSound es (i, x)
requestToEvent es (AddLocation x) = fireAddLocation es x
requestToEvent es (RemoveLocation i) = fireRemoveLocation es i
requestToEvent es (UpdateLocation i f) = fireUpdateLocation es (i, f)
requestToEvent es (AddListener x) = fireAddListener es x
requestToEvent es (RemoveListener i) = fireRemoveListener es i
requestToEvent es (UpdateListener i f) = fireUpdateListener es (i, f)
requestToEvent es Quit = fireQuit es ()

arguments :: Mode Options
arguments =
    mode "streamero-sound-engine" defaultOptions "Streamero Sound Engine v0.1"
         (flagArg (upd socket . Just) "SOCKET") $
         [ flagHelpSimple (setL help True)
         , flagReq ["jack-path"] (upd jackPath) "STRING" "Path to Jack command"
         , flagReq ["jack-driver", "d"] (upd jackDriver) "STRING" "Jack driver"
         , flagReq ["udp-port"] (upd scUdpPort . read) "INTEGER" "SuperCollider UDP port"
         , flagBool ["monitor"] (setL monitor) "Whether to monitor the stream locally"
         , flagReq ["max-num-listeners", "n"] (upd maxNumListeners . read) "INTEGER" "Maximum number of listeners"
         , flagBool ["local"] (setL local) "Stream to a local instance of icecast"
         ]
    where
        upd what x = Right . setL what x

withSC :: Environment -> String -> SC.Server a -> IO a
withSC env clientName =
    SC.withSynth
        SC.defaultServerOptions {
            SC.numberOfInputBusChannels = 2
          , SC.numberOfOutputBusChannels = 2 * (maxNumListeners . options ^$ env)
          {-, SC.ugenPluginPath = Just [ "./plugins" ]-}
          }
        SC.defaultRTOptions {
            SC.networkPort = SC.UDPPort (scUdpPort . options ^$ env)
          , SC.hardwareDeviceName = Just $ (jackServerName ^$ env) ++ ":" ++ clientName
          }
        SC.defaultOutputHandler { SC.onPutString = logLn SC
                                , SC.onPutError = logLn SC }

withUnixSocket :: String -> (N.Socket -> IO a) -> IO a
withUnixSocket file = E.bracket
    (do { s <- N.socket N.AF_UNIX N.Stream 0
        ; N.connect s (N.SockAddrUnix file)
        ; return s
        })
    N.sClose

-- --------------------------------------------------------------------
-- HashMap helpers

lookupTag :: (Eq k, H.Hashable k, Show k) => String -> k -> H.HashMap k v -> v
lookupTag t k = H.lookupDefault (error (t ++ ": key " ++ show k ++ " not found")) k

forMWithKey_ :: (Eq k, H.Hashable k, Monad m) => H.HashMap k v -> (k -> v -> m ()) -> m ()
forMWithKey_ h f = F.forM_ (H.keys h) $ \k -> f k (h H.! k)

-- --------------------------------------------------------------------
-- Logging

data Component = ENGINE
               | STREAMER
               | SC
               | JACK
               deriving (Show)

logLn :: Component -> String -> IO ()
logLn component s = putStrLn $ "[" ++ show component ++ "] " ++ s

logLn_ :: String -> IO ()
logLn_ = logLn ENGINE

logE :: Component -> R.Event t String -> R.NetworkDescription t ()
logE c = R.reactimate . fmap (logLn c)

logE_ :: R.Event t String -> R.NetworkDescription t ()
logE_ = logE ENGINE

traceE :: String -> R.Event t String -> R.NetworkDescription t ()
traceE tag = logE_ . fmap ((tag++": ")++)

traceShowE :: Show a => String -> R.Event t a -> R.NetworkDescription t ()
traceShowE tag = traceE tag . fmap show

-- --------------------------------------------------------------------
-- (A)synchronous monadic actions and events

newSyncEvent :: R.NetworkDescription t (R.Event t a, IO a -> IO ())
newSyncEvent = do
    (evt, fire) <- R.newEvent
    return (evt, \action -> action >>= fire)

reactimateMS :: MonadIO m => MonadServer.Handle m -> (a -> IO ()) -> R.Event t (m a) -> R.NetworkDescription t ()
reactimateMS handle sink = R.reactimate . fmap (MonadServer.executeWith handle sink)

newAsyncEvent :: MonadIO m => MonadServer.Handle m -> R.Event t (m a) -> R.NetworkDescription t (R.Event t a)
newAsyncEvent handle evt = do
    (evt', fire) <- R.newEvent
    reactimateMS handle fire evt
    return evt'

execute' :: (SC.ServerT IO () -> IO ()) -> (a -> IO ()) -> SC.ServerT IO a -> IO ()
execute' f g m = f (m >>= liftIO . g)

try :: IO a -> IO (Either Exception a)
try = E.try

-- --------------------------------------------------------------------
-- Reactive helpers

scanlE :: (a -> b -> a) -> a -> R.Event t b -> R.Event t a
scanlE f a0 = R.accumE a0 . fmap (flip f)

sample :: R.Behavior t a -> R.Event t b -> R.Event t (a, b)
sample = R.apply . fmap (,)

sample_ :: R.Behavior t a -> R.Event t b -> R.Event t a
sample_ = (<@)

zipB :: R.Behavior t a -> R.Behavior t b -> R.Behavior t (a, b)
zipB a b = (,) <$> a <*> b

lookupB :: (Eq k, H.Hashable k) => R.Behavior t (H.HashMap k a) -> R.Event t k -> R.Event t (k, a)
lookupB h = R.filterJust . R.apply ((\m k -> fmap ((,)k) (H.lookup k m)) <$> h)

lookupB_ :: (Eq k, H.Hashable k) => R.Behavior t (H.HashMap k a) -> R.Event t k -> R.Event t a
lookupB_ h = fmap snd . lookupB h

hashMapB :: (Eq k, H.Hashable k) => R.Event t (k, v) -> R.Event t k -> R.Event t (k, v -> v) -> (R.Event t (H.HashMap k v), R.Behavior t (H.HashMap k v))
hashMapB insert remove update =
  let e = (uncurry H.insert <$> insert)
          `R.union`
          (H.delete <$> remove)
          `R.union`
          (uncurry (flip H.adjust) <$> update)
      dup x = (x, x)
      result = fmap
  in R.mapAccum H.empty (result dup <$> e)

-- --------------------------------------------------------------------
-- Sounds and sound files

soundFileInfo :: FilePath -> IO SoundFileInfo
soundFileInfo path = do
    info <- SF.getInfo path
    return $ SoundFileInfo (SF.samplerate info) (SF.channels info) (fromIntegral $ SF.frames info)

duration :: SoundFileInfo -> Double
duration i = fromIntegral (numFrames i) / fromIntegral (sampleRate i)

type SoundMap = H.HashMap SoundId (Sound, SoundFileInfo)

lookupSounds :: [SoundId] -> SoundMap -> SoundMap
--lookupSounds ids = flip H.intersection . H.fromList (map ((,)()) ids)
lookupSounds ids soundMap = List.foldl' (\h i -> case H.lookup i soundMap of
                                                    Nothing -> h
                                                    Just s -> H.insert i s h)
                                        H.empty
                                        ids

lookupSoundsOf :: (Sound -> Bool) -> [SoundId] -> SoundMap -> SoundMap
lookupSoundsOf f ids = H.filter (f . fst) . lookupSounds ids

unsupportedFormat :: SF.Exception -> Maybe SF.Exception
unsupportedFormat e =
    case SF.errorString e of
        "File contains data in an unknown format." -> Just e
        _ -> Nothing

fromSndfileException :: FilePath -> SF.Exception -> Exception
fromSndfileException path = FileError path . SF.errorString

addSound :: Sound -> IO (Sound, SoundFileInfo)
addSound s =
  E.handle (E.throw . fromSndfileException file) $ do
    E.catchJust unsupportedFormat (((,)s) <$> soundFileInfo file) $ \e -> do
      let cacheDir = "tmp/cache/streamero-sound-engine"
          cacheFile = cacheDir </> show (soundId s) ++ ".flac"
      cacheFileExists <- Dir.doesFileExist cacheFile
      unless cacheFileExists $ do
        Dir.createDirectoryIfMissing True cacheDir
        SF.toFLAC file cacheFile
      ((,) s { soundType = (soundType s) { path = cacheFile } })
        <$> soundFileInfo cacheFile
  where file = path (soundType s)

-- --------------------------------------------------------------------
-- Players

data Player = Player SC.Buffer SC.Synth deriving Show

playerSynthDef :: Int -> SC.Loop -> SC.UGen
playerSynthDef nc loop =
    SC.out (SC.control SC.KR "out" 0)
  $ toStereo
  $ SC.diskIn nc (SC.control SC.IR "buffer" (-1)) loop

mkPlayerSynthDef :: Int -> SC.Loop -> SC.ServerT IO SC.SynthDef
mkPlayerSynthDef nc loop = SC.exec' immediately . SC.async . SC.d_recv ("player-" ++ show loop ++ "-" ++ show nc) . playerSynthDef nc $ loop

truncatePowerOfTwo :: (Bits.Bits a, Integral a) => a -> a
truncatePowerOfTwo = Bits.shiftL 1 . truncate . logBase (2::Double) . fromIntegral

diskBufferSize :: SoundFileInfo -> Int
diskBufferSize = fromIntegral . min 32768 . truncatePowerOfTwo . numFrames 

newPlayer :: (Int -> SC.Loop -> SC.ServerT IO SC.SynthDef) -> SC.AudioBus -> Sound -> SoundFileInfo -> SC.ServerT IO Player
newPlayer mkSynthDef bus sound info = do
    let nc = numChannels info
    sd <- mkSynthDef nc (if isEvent sound then SC.NoLoop else SC.Loop)
    g <- SC.rootNode
    {-SC.exec' immediately $ SC.dumpOSC SC.TextPrinter-}
    (buffer, synth) <- SC.exec' immediately $
        SC.b_alloc (diskBufferSize info) nc `SC.whenDone` \buffer ->
            SC.b_read buffer (path (soundType sound)) Nothing Nothing Nothing True `SC.whenDone` \_ -> do
                -- FIXME: Put all player synths in one group, see TODO above
                synth <- SC.s_new sd SC.AddToHead g
                            [ control "buffer" buffer
                            , control "out" bus ]
                SC.resource (buffer, synth)
    return $ Player buffer synth

freePlayer :: (Functor m, MonadIO m) => Player -> SC.RequestT m ()
freePlayer (Player buffer synth) = SC.n_free synth >> void (SC.async (SC.b_free buffer))

-- | Play an event sound at a certain location.
playSound :: (Listener, ListenerState) -> (Sound, SoundFileInfo) -> SC.ServerT IO Player
playSound (listener, listenerState) (sound, soundFileInfo) = do
    player <- newPlayer mkPlayerSynthDef (outputBus listenerState) sound soundFileInfo
    void $ SC.fork $ do
        liftIO $ OSC.pauseThread (duration soundFileInfo)
        SC.exec immediately $ do
            let Player buffer synth = player
            SC.n_free synth
            SC.async $ SC.b_free buffer
            return ()
        {-liftIO $ logLn "Event player freed"-}
    return player

-- --------------------------------------------------------------------
-- Locations

addLocation :: SoundMap -> Location -> SC.ServerT IO (Location, LocationState)
addLocation soundMap location = do
    bus <- SC.newAudioBus 2
    let sounds = lookupSoundsOf isContinuous (locationSounds location) soundMap
    players <- T.mapM (uncurry (newPlayer mkPlayerSynthDef bus)) sounds
    return (location, LocationState bus players)

removeLocation :: (Location, LocationState) -> SC.ServerT IO LocationId
removeLocation (location, state) = do
    -- Free synths
    SC.exec immediately $ F.mapM_ freePlayer (players state)
    -- Free output bus
    SC.freeBus $ bus state
    return $ locationId location

updateLocation :: SoundMap -> (Location, LocationState) -> SC.ServerT IO (Location, LocationState)
updateLocation soundMap (location, state) = do
    -- Find ids of continuous sounds not yet playing
    let locationSoundMap = H.fromList (map (flip(,)()) (locationSounds location))
        continuous = H.filterWithKey (\k _ -> maybe False (isContinuous.fst) (H.lookup k soundMap))
        continuousSounds = continuous . flip lookupSounds soundMap
        diff a b = H.keys (H.difference a b)
        soundsToPlay = lookupSounds (H.keys $ continuous (H.difference locationSoundMap (players state))) soundMap
        playersToStop = continuous (H.difference (players state) locationSoundMap)
    -- Start new players
    newPlayers <- T.mapM (uncurry (newPlayer mkPlayerSynthDef (bus state))) soundsToPlay
    -- Stop old players
    SC.exec immediately $ F.mapM_ freePlayer playersToStop
    --liftIO $ logStrLn "updateLocation" $ show (locationSounds location, H.keys soundsToPlay, H.keys playersToStop, newPlayers)
    return (location, state { players = H.union newPlayers (H.filterWithKey (\k _ -> not (H.member k playersToStop)) (players state)) })

updateLocationsForSound :: LocationMap -> (Sound, SoundFileInfo) -> SC.ServerT IO (LocationMap -> LocationMap)
updateLocationsForSound locations (sound, soundFileInfo)
  | isEvent sound = do
    locations' <- flip H.traverseWithKey locations $ \_ (location, locationState) ->
      case H.lookup (soundId sound) (players locationState) of
        Nothing -> return (location, locationState)
        Just player -> do
          SC.exec immediately $ freePlayer player
          return $ (location, locationState { players = H.delete (soundId sound) (players locationState) })
    return $ const locations'
  | isContinuous sound = do
    locations' <- flip H.traverseWithKey locations $ \_ (location, locationState) ->
      case H.lookup (soundId sound) (players locationState) of
        Nothing -> if elem (soundId sound) (locationSounds location)
                   then do
                      -- Start player
                      player <- newPlayer mkPlayerSynthDef (bus locationState) sound soundFileInfo
                      return (location, locationState { players = H.insert (soundId sound) player (players locationState) })
                   else return (location, locationState)
        Just _ ->
          -- Already playing
          return (location, locationState)
    return $ const locations'
  | otherwise = return id

-- Gain is 1 for dist < refDist and 0 for dist > maxDist.
-- This is an extension of the OpenAL "Inverse Distance Clamped Model" (equivalent to the )
distanceScaling :: Double -> Double -> Double -> Double -> Double
distanceScaling rolloffFactor refDist maxDist dist
    | dist > maxDist = 0
    | otherwise      = refDist / (refDist + rolloffFactor * (dist' - refDist))
        where dist' = min (max refDist dist) maxDist

locationDistanceScaling :: Location -> Double -> Double
locationDistanceScaling = distanceScaling 0.1 1 . radius
--locationDistanceScaling _ _ = 1.0

-- --------------------------------------------------------------------
-- Streaming client

newtype StreamId = StreamId Int deriving (Eq, Ord, Show)

streamName :: StreamId -> String
streamName (StreamId i) = "stream-" ++ show i

streamBusId :: StreamId -> Int
streamBusId (StreamId i) = i * 2

streamId :: SC.AudioBus -> StreamId
streamId b = StreamId (fromIntegral (SC.busId b) `div` 2)

streamIds :: Environment -> [StreamId]
streamIds env = map StreamId [0..getL (maxNumListeners.options) env - 1]

-- TODO: Use control-monad-exception for exception reporting
--       There also needs to be a way to communicate exceptions back from
--       the SC engine loop to the reactive event network.
findStreamId :: Failure SC.AllocFailure m => Environment -> ListenerMap -> m StreamId
findStreamId env listeners = do
    case avail List.\\ used of
        [] -> failure SC.NoFreeIds
        (i:_) -> return i
    where used  = map (streamId.outputBus.snd) (H.elems listeners)
          avail = streamIds env

startDarkice :: Environment -> String -> String -> URL -> IO ProcessHandle
startDarkice env jackName streamName url = do
    FS.writeTextFile darkiceCfgFile $ Darkice.configText darkiceCfg
    SProc.createProcess (logLn STREAMER . (("[" ++ streamName++"] ")++)) (Darkice.createProcess "darkice" darkiceOpts)
    where darkiceCfgFile = FS.append (temporaryDirectory ^$ env) (FS.decodeString $ jackName ++ ".cfg")
          darkiceCfg = Darkice.defaultConfig {
                          Darkice.jackClientName = Just jackName
                        , Darkice.server =
                            case URL.url_type url of
                              URL.Absolute host -> URL.host host
                              _ -> Darkice.server Darkice.defaultConfig
                        , Darkice.port =
                            case URL.url_type url of
                            URL.Absolute host -> maybe (Darkice.port Darkice.defaultConfig)
                                                       fromIntegral
                                                       (URL.port host)
                            _ -> Darkice.port Darkice.defaultConfig
                        , Darkice.password = "h3aRh3aR"
                        , Darkice.mountPoint = URL.url_path url
                        , Darkice.bufferSize = 1
                        , Darkice.name = Text.pack streamName
                        , Darkice.genre = "Soundscape" }
          darkiceOpts = Darkice.defaultOptions {
                          Darkice.configFile = Just darkiceCfgFile }
          --escapePath = map (\c -> if c == '/' then '-' else c)

makeConnectionMap :: Bool -> Environment -> SJack.Connections
makeConnectionMap monitor = concatMap f . streamIds
    where
        f i = let stream = streamName i
                  stream1 = "left"
                  stream2 = "right"
                  sc = "supercollider"
                  sc1 = "out_" ++ show (streamBusId i + 1)
                  sc2 = "out_" ++ show (streamBusId i + 2)
              in [ ((sc,sc1),(stream,stream1))
                 , ((sc,sc2),(stream,stream2)) ]
                 ++ if monitor
                    then [ ((sc,sc1),("system","playback_1"))
                         , ((sc,sc2),("system","playback_2")) ]
                    else []

-- --------------------------------------------------------------------
-- Listeners

listenerLocationDistance :: Listener -> Location -> Double
listenerLocationDistance li lo =
  locationDistance lo (listenerPosition li)

listenerLocationDistanceScaling :: Listener -> Location -> Double
listenerLocationDistanceScaling li lo =
  locationDistanceScaling lo (listenerLocationDistance li lo)

listenerLocations :: Listener -> LocationMap -> LocationMap
listenerLocations listener =
  H.filter $ \(location, _) ->
                listenerLocationDistance listener location
                  <= radius location

addListener :: Environment -> ListenerMap -> Listener -> SC.ServerT IO (Listener, ListenerState)
addListener env listeners listener = do
    sid <- findStreamId env listeners
    let url = streamURL listener
    darkice <- liftIO $ startDarkice
                          env
                          (streamName sid)
                          (URL.url_path url)
                          url
    output <- SC.outputBus 2 (streamBusId sid)
    return (listener, ListenerState output darkice)

removeListener :: ListenerMap -> ListenerId -> SC.ServerT IO ListenerId
removeListener listeners listenerId = do
  let state = snd (lookupTag "removeListener" listenerId listeners)
  -- Free output bus
  SC.freeBus $ outputBus state
  -- Stop stream
  liftIO $ do
      SProc.interruptProcess (streamer state)
      logLn STREAMER $ "Stopped stream"
  return listenerId

removeAllListeners :: ListenerMap -> SC.ServerT IO ()
removeAllListeners listeners = mapM_ (removeListener listeners) (H.keys listeners)

-- --------------------------------------------------------------------
-- Patch cables

type PatchCables = H.HashMap ListenerId (H.HashMap LocationId SC.Synth)

patchCableSynthDef :: SC.UGen
patchCableSynthDef =
    let i = SC.in' 2 SC.AR (SC.control SC.KR "in" 0)
        o = SC.out (SC.control SC.KR "out" 0)
    in o (i * SC.lag (SC.control SC.KR "level" 0) 0.1)

newPatchCable :: SC.Group -> SC.AudioBus -> SC.AudioBus -> Listener -> Location -> SC.ServerT IO SC.Synth
newPatchCable group input output listener location = do
    -- TODO: Only send SynthDef once
    sd <- SC.exec' immediately . SC.async $ SC.d_recv "patchCable" patchCableSynthDef
    let level = listenerLocationDistanceScaling listener location
    SC.exec immediately $ SC.s_new sd SC.AddToTail group
                            [ control "in" input
                            , control "out" output
                            , control "level" level ]

updatePatchCablesForListener :: LocationMap -> PatchCables -> Listener -> SC.ServerT IO (PatchCables -> PatchCables)
updatePatchCablesForListener locations patchCables listener = do
  case H.lookup (listenerId listener) patchCables of
    Nothing -> return id
    Just listenerPatchCables -> do
      forMWithKey_ listenerPatchCables $ \locationId synth ->
        let location = fst $ lookupTag "updatePatchCablesForListener"
                                       locationId
                                       locations
            level = listenerLocationDistanceScaling listener location
        --liftIO $ logStrLn "updatePatchCablesForListener" $ "Distance: " ++ show (listenerPosition listener) ++ " " ++ show (listenerId, dist, level)
        in SC.exec immediately $ SC.n_set synth [ control "level" level ]
      return id

updatePatchCablesForLocation :: ListenerMap -> PatchCables -> Location -> SC.ServerT IO (PatchCables -> PatchCables)
updatePatchCablesForLocation listeners patchCables location = do
  forMWithKey_ patchCables $ \listenerId cables ->
    case H.lookup (locationId location) cables of
      Just synth ->
        let listener = fst $ lookupTag "updatePatchCablesForLocation" listenerId listeners
            level = listenerLocationDistanceScaling listener location
        in SC.exec immediately $ SC.n_set synth [ control "level" level ]
  return id

-- --------------------------------------------------------------------
-- Event sounds and players

data LocationEvent = Enter Listener ListenerState Location
                   | Leave Listener Location
                   deriving Show

data EventPlayer = EventPlayer SC.AudioBus SC.Synth (H.HashMap SoundId Player) deriving Show
type EventPlayerMap = H.HashMap ListenerId (H.HashMap LocationId EventPlayer)

updateEventPlayers :: SoundMap -> EventPlayerMap -> LocationEvent -> SC.ServerT IO (EventPlayerMap -> EventPlayerMap)
updateEventPlayers sounds eventPlayers event =
    case event of
        Enter listener listenerState location ->
            let eventSounds = H.filter (isEvent.fst) (lookupSounds (locationSounds location) sounds)
                level = listenerLocationDistanceScaling listener location
            in if H.null eventSounds
               then return id
               else do
                bus <- SC.newAudioBus 2
                players <- T.mapM (uncurry (newPlayer mkPlayerSynthDef bus)) eventSounds
                g <- SC.rootNode
                cable <- newPatchCable g bus (outputBus listenerState) listener location
                return $ H.insert (listenerId listener)
                                  (H.insert (locationId location)
                                            (EventPlayer bus cable players)
                                            (H.lookupDefault H.empty (listenerId listener) eventPlayers))
        Leave listener location ->
            case H.lookup (listenerId listener) eventPlayers of
                Nothing -> return id
                Just h -> case H.lookup (locationId location) h of
                            Nothing -> return id
                            Just (EventPlayer bus cable players) -> do
                                SC.exec immediately $ do
                                    F.mapM_ freePlayer players
                                    SC.n_free cable
                                SC.freeBus bus
                                return $ H.insert (listenerId listener) (H.delete (locationId location) h)

updateEventPlayersForListener :: LocationMap -> EventPlayerMap -> Listener -> SC.ServerT IO (EventPlayerMap -> EventPlayerMap)
updateEventPlayersForListener locations eventPlayers listener = do
    case H.lookup (listenerId listener) eventPlayers of
        Nothing -> return ()
        Just es -> forMWithKey_ es $ \locationId (EventPlayer _ cable _) ->
            let level = listenerLocationDistanceScaling
                          listener
                          (fst (lookupTag "updateEventPlayersForListener" locationId locations))
            in SC.exec immediately $ SC.n_set cable [ control "level" level ]
    return id

-- | Stop all event players for (previously) continuous sound.
updateEventPlayersForSound :: EventPlayerMap -> Sound -> SC.ServerT IO (EventPlayerMap -> EventPlayerMap)
updateEventPlayersForSound eventPlayers sound
  | isContinuous sound = do
      eventPlayers' <- flip H.traverseWithKey eventPlayers $ \listenerId locations ->
        flip H.traverseWithKey locations $ \locationId e@(EventPlayer b s players) ->
          case H.lookup (soundId sound) players of
            Nothing -> return e
            Just player -> do
              SC.exec immediately $ freePlayer player
              return $ EventPlayer b s (H.delete (soundId sound) players)
      return $ const eventPlayers'
  | otherwise = return id

-- --------------------------------------------------------------------
-- Main engine function

main :: IO ()
main = do
    hSetBuffering stdout LineBuffering
    opts <- processArgs arguments
    if help ^$ opts
        then print $ helpText [] HelpFormatDefault arguments
        else do
            withTemporaryDirectory "streamero_" $ \tmpDir_ -> do
                let tmpDir = FS.decodeString tmpDir_
                    jackOptions = (SJack.defaultOptions (FS.encodeString $ FS.basename tmpDir)) {
                                        SJack.driver = jackDriver ^$ opts
                                      , SJack.timeout = Just 5000
                                      , SJack.temporary = False
                                      }
                logLn_ $ "Starting with temporary directory " ++ tmpDir_
                SJack.withJack (logLn JACK) (jackPath ^$ opts) jackOptions $ \serverName -> do
                    logLn_ $ "Jack server name: " ++ serverName
                    let env = makeEnv opts tmpDir serverName
                        connectionMap = makeConnectionMap (monitor ^$ opts) env
                        handledSignals = [sigINT, sigTERM]
                        run source sink = do
                            engine <- MonadServer.new (ignoreSignals handledSignals . withSC env "supercollider")
                            let reactimateEngine = reactimateMS engine

                            {-quitVar <- MVar.newEmptyMVar-}
                            fromEngineVar <- MVar.newEmptyMVar
                            eventSinks <- MVar.newEmptyMVar
                            let --toEngine = executeE engine
                                fromEngine = MVar.takeMVar fromEngineVar
                                send = MVar.putMVar fromEngineVar
                                {-quitEngine = MVar.putMVar quitVar ()-}
                                {-quitEngine = quit engine-}
                            {-withSC env "supercollider" $ do-}
                                {-toEngine <- SC.capture-}
                            let networkDescription :: forall t . R.NetworkDescription t ()
                                networkDescription = do

                                -- Create input events and sinks
                                (input, sinks) <- mkInputEvents
                                liftIO $ MVar.putMVar eventSinks sinks

                                -- AddSound
                                -- Read sound file info synchronously
                                ((eAddSoundError, eAddSound'), fAddSound') <- first R.split <$> newSyncEvent
                                -- FIXME: Catch exception and report error when file not found.
                                R.reactimate $ fAddSound' . try . addSound <$> eAddSound input

                                -- Update sound map
                                let eUpdateSound' = (\h (k, f) -> first f (lookupTag "eUpdateSound" k h))
                                                    <$> bSounds
                                                    <@> eUpdateSound input
                                    (eSounds, bSounds) = R.mapAccum H.empty $ R.unions [
                                        (\(s, s') acc -> let acc' = H.insert (soundId s) (s, s') acc in (acc', acc')) <$> eAddSound'
                                      , (\(s, s') acc -> let acc' = H.insert (soundId s) (s, s') acc in (acc', acc')) <$> eUpdateSound'
                                      ]

                                R.reactimate $ send Ok <$ R.unions [ eAddSound', eUpdateSound' ]
                                R.reactimate $ send . Error <$> eAddSoundError

                                traceShowE "AddSound" $ R.union (Left <$> eAddSoundError) (Right <$> eAddSound')
                                traceShowE "Sounds" eSounds

                                -- Locations and states
                                (eAddLocation', fAddLocation') <- R.newEvent
                                (eRemoveLocation', fRemoveLocation') <- R.newEvent
                                (eUpdateLocation', fUpdateLocation') <- R.newEvent

                                (eUpdateLocationsForSound, fUpdateLocationsForSound) <- R.newEvent

                                let bLocations = R.accumB
                                                  H.empty
                                                  (R.unions [ (\(l, s) -> H.insert (locationId l) (l, s))
                                                              <$> R.unions [
                                                                    eAddLocation'
                                                                  , eUpdateLocation' ]
                                                            , H.delete <$> eRemoveLocation'
                                                            , eUpdateLocationsForSound
                                                            ])
                                eLocations <- R.changes bLocations

                                reactimateEngine fAddLocation' $
                                  addLocation
                                  <$> bSounds
                                  <@> eAddLocation input
                                reactimateEngine fRemoveLocation' $
                                  removeLocation
                                  <$> bLocations `lookupB_` eRemoveLocation input
                                reactimateEngine fUpdateLocation' $
                                  updateLocation
                                  <$> bSounds
                                  <@> ((\h (k, f) -> first f (lookupTag "eUpdateLocation" k h))
                                       <$> bLocations
                                       <@> eUpdateLocation input)

                                R.reactimate $ send Ok <$ R.unions [ void eAddLocation'
                                                                   , void eRemoveLocation'
                                                                   , void eUpdateLocation' ]

                                traceShowE "AddLocation" (eAddLocation input)
                                traceShowE "UpdateLocation" (fst <$> eUpdateLocation input)
                                traceShowE "RemoveLocation" (eRemoveLocation input)
                                traceShowE "Locations" eLocations

                                -- Update locations when sound isEvent field changes
                                reactimateEngine fUpdateLocationsForSound $
                                  updateLocationsForSound
                                  <$> bLocations
                                  <@> eUpdateSound'

                                -- Listeners and listener states
                                let eAddListenerRewriteUrl = flip fmap (eAddListener input) $
                                      if local ^$ opts
                                      then (\l -> l { streamURL = setHostName "localhost" (streamURL l) })
                                      else id
                                R.reactimate $ logLn_ . URL.exportURL . streamURL <$> eAddListenerRewriteUrl

                                (eAddListener', fAddListener') <- R.newEvent
                                (eRemoveListener', fRemoveListener') <- R.newEvent
                                -- No side effects
                                --let eUpdateListener' = (\(i, f) -> (i, first f)) <$> eUpdateListener
                                let eUpdateListener' = (\ls (i, f) -> first f (lookupTag "eUpdateListener" i ls))
                                                       <$> bListeners
                                                       <@> eUpdateListener input
                                    bListeners = R.accumB
                                                  H.empty
                                                  (R.unions [ (\(l, s) -> H.insert (listenerId l) (l, s)) <$> eAddListener'
                                                            , (\(l, s) -> H.insert (listenerId l) (l, s)) <$> eUpdateListener'
                                                            , H.delete <$> eRemoveListener' ])
                                eListeners <- R.changes bListeners

                                reactimateEngine fAddListener' $
                                  addListener env
                                  <$> bListeners
                                  <@> eAddListenerRewriteUrl

                                reactimateEngine fRemoveListener' $
                                  removeListener
                                  <$> bListeners
                                  <@> eRemoveListener input

                                R.reactimate $ send Ok <$ R.unions [ void eAddListener'
                                                                   , void eRemoveListener'
                                                                   , void eUpdateListener' ]

                                traceShowE "AddListener" eAddListener'
                                --traceShowE "eUpdateListener" (fst <$> eUpdateListener)
                                traceShowE "listenerPosition" (listenerPosition . fst <$> eUpdateListener')
                                traceShowE "RemoveListener" eRemoveListener'

                                -- Listener/location map
                                let (eLocationEvents, bListenerLocations) =
                                        first R.spill
                                      $ R.mapAccum H.empty
                                      $ R.unions [
                                        (\locs (l, state) acc ->
                                          let i  = listenerId l
                                              ls = fmap fst . H.elems $ listenerLocations l locs
                                              s  = HS.fromList ls
                                              es = case H.lookup i acc of
                                                    Nothing -> Enter l state <$> ls
                                                    Just s' -> (Leave l <$> HS.toList (HS.difference s' s))
                                                            ++ (Enter l state <$> HS.toList (HS.difference s s'))
                                          in (es, H.insert i s acc))
                                        <$> bLocations <@> eUpdateListener'
                                      , (\(l, _) acc ->
                                          let i = listenerId l
                                          in case H.lookup i acc of
                                              Nothing -> ([], acc)
                                              Just s  -> (Leave l <$> HS.toList s, H.delete i acc))
                                        <$> (flip (lookupTag "eLocationEvents") <$> bListeners <@> eRemoveListener') ]

                                --traceShowE "LocationEvents" eLocationEvents
                                traceE "LocationEvents" $
                                       (\e -> case e of
                                                Enter li _ lo -> unwords ["Enter", show (listenerId li), show (locationId lo)]
                                                Leave li lo -> unwords ["Leave", show (listenerId li), show (locationId lo)] )
                                       <$> eLocationEvents
                                --traceShowE "bListenerLocations" (const <$> bListenerLocations <@> eUpdateListener')

                                -- Start and stop event players
                                (eUpdateEventPlayers, fUpdateEventPlayers) <- R.newEvent
                                let bEventPlayers = R.accumB H.empty eUpdateEventPlayers
                                eEventPlayers <- R.changes bEventPlayers

                                reactimateEngine fUpdateEventPlayers $
                                  updateEventPlayers
                                  <$> bSounds
                                  <*> bEventPlayers
                                  <@> eLocationEvents

                                -- Update event players
                                reactimateEngine fUpdateEventPlayers $
                                  updateEventPlayersForListener
                                  <$> bLocations
                                  <*> bEventPlayers
                                  <@> fmap fst eUpdateListener'

                                -- Update event players when isEvent field changes
                                reactimateEngine fUpdateEventPlayers $
                                  updateEventPlayersForSound
                                  <$> bEventPlayers
                                  <@> fmap fst eUpdateSound'

                                traceShowE "EventPlayers" eEventPlayers

                                -- Piggedipatchables
                                (eUpdatePatchCables, fUpdatePatchCables) <- R.newEvent
                                let bPatchCables = R.accumB H.empty eUpdatePatchCables
                                    eUpdatePatchCables' = R.unions [
                                        -- AddListener: Add patch cables between new listener and all locations
                                        (\locations (listener, listenerState) -> do
                                          group <- SC.rootNode
                                          cables <- T.forM locations $ \(location, locationState) ->
                                            newPatchCable group
                                                          (bus locationState)
                                                          (outputBus listenerState)
                                                          listener
                                                          location
                                          return $ H.insert (listenerId listener) cables)
                                        <$> bLocations
                                        <@> eAddListener'
                                        -- UpdateListener: Update patch cables with new listener position
                                      , updatePatchCablesForListener
                                        <$> bLocations
                                        <*> bPatchCables
                                        <@> fmap fst eUpdateListener'
                                        -- RemoveListener: Remove patch cables between listener and all locations
                                      , (\patchCables listenerId -> do
                                          SC.exec immediately $ F.mapM_ SC.n_free
                                                              $ lookupTag "eUpdatePatchCables'" listenerId patchCables
                                          return $ H.delete listenerId)
                                        <$> bPatchCables
                                        <@> eRemoveListener'
                                        -- AddLocation: Add patch cables between the new location and each listener
                                      , (\listeners (location, locationState) -> do
                                          group <- SC.rootNode
                                          updates <- T.forM listeners $ \(listener, listenerState) -> do
                                            synth <- newPatchCable
                                                      group
                                                      (bus locationState)
                                                      (outputBus listenerState)
                                                      listener
                                                      location
                                            return $ H.insert (locationId location) synth
                                          return $ flip (H.foldlWithKey' (\h k f -> H.adjust f k h))
                                                        updates)
                                        <$> bListeners
                                        <@> eAddLocation'
                                        -- UpdateLocation: Update patch cables with new location position
                                      , updatePatchCablesForLocation
                                        <$> bListeners
                                        <*> bPatchCables
                                        <@> fmap fst eUpdateLocation'
                                        -- RemoveLocation: Remove patch cables netween the location and each listener
                                      , (\patchCables locationId -> do
                                          SC.exec immediately $ mapM_ SC.n_free
                                                              $ catMaybes $ H.elems
                                                              $ fmap (H.lookup locationId) patchCables
                                          return $ fmap (H.delete locationId))
                                        <$> bPatchCables
                                        <@> eRemoveLocation'
                                      ]
                                reactimateEngine fUpdatePatchCables eUpdatePatchCables'
                                traceShowE "PatchCables" =<< R.changes bPatchCables

                                -- Quit
                                (eFreedListeners, fFreedListeners) <- R.newEvent
                                reactimateEngine fFreedListeners $
                                  (const <$> removeAllListeners)
                                  <$> bListeners
                                  <@> eQuit input
                                R.reactimate $ (MonadServer.quit engine >> send Ok) <$ eFreedListeners
                            -- Compile network and get event sinks
                            network <- R.compile networkDescription
                            sinks <- MVar.takeMVar eventSinks
                            -- Set up interrupt handler
                            catchSignals handledSignals $ fireQuit sinks ()
                            -- Activate network
                            R.actuate network
                            -- Sync with engine
                            {-execute engine $ SC.exec' immediately $ SC.dumpOSC SC.TextPrinter-}
                            MonadServer.execute engine $ return ()
                            -- Set up connections to and from JSON I/O network
                            void $ forkIO $ source =$= conduitIO (requestToEvent sinks) fromEngine $$ sink
                            {-MVar.takeMVar quitVar-}
                            MonadServer.wait engine
                            -- Deactivate network
                            R.pause network
                    liftIO $ do
                        let delay = 1
                        logLn_ $ "Waiting " ++ show delay ++ " seconds before trying to connect to Jack"
                        OSC.pauseThread delay
                    void $ SJack.withPatchBay serverName "patchbay" connectionMap $ \client -> do
                        lift $ case socket ^$ opts of
                            {-Nothing ->-}
                                {--- Readline interface-}
                                {-let source = sourceReadline "> " =$= CL.map BS8.pack =$= parseJsonStream =$= fromJson-}
                                    {-sink = toJson =$= unlines =$= CB.sinkHandle stdout-}
                                {-in run source sink-}
                            Nothing ->
                                -- Terminal interface
                                let source = CB.sourceHandle stdin =$= C.mapOutput (BC.fromChunks . (:[])) CB.lines =$= parseJsonMessage =$= {- printC =$= -} fromJson
                                    sink = toJson =$= unlines =$= CB.sinkHandle stdout
                                in run source sink
                            Just socketFile ->
                                -- Socket interface
                                withUnixSocket socketFile $ \s ->
                                    let source = C.sourceSocket s =$= message =$= parseJsonMessage =$= {- printC =$= -} fromJson
                                        sink = toJson =$= unmessage =$= C.sinkSocket s
                                    in run source sink
            logLn_ "done."
