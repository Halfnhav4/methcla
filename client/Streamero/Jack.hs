{-# LANGUAGE RankNTypes #-}
module Streamero.Jack (
    Options(..)
  , defaultOptions
  , optionList
  , createProcess
  , openPatchBay
  , closePatchBay
) where

{-import           Data.ByteString (ByteString)-}
{-import           Data.Conduit.Process-}
{-import qualified Data.Conduit as C-}
import           Control.Applicative
import           Control.Concurrent (forkIO)
import           Control.Concurrent.MVar
import           Control.Monad.IO.Class (liftIO)
import qualified Control.Monad.Trans.Class as Trans
import           Data.Word (Word32)
import           System.Process (CreateProcess, proc)
import qualified Data.HashMap.Strict as Map
import qualified Data.HashSet as Set
import           Data.List.Split (splitOn)
import           Foreign.C
import           Foreign.Ptr
import qualified Reactive.Banana as R
import qualified Sound.JACK as JACK

data Options = Options {
    name :: String
  , driver :: String
  , verbose :: Bool
  , temporary :: Bool
  , rate :: Word32
  , period :: Word32
  } deriving (Show)

defaultOptions :: String -> Options
defaultOptions n = Options {
    name = n
  , driver = "dummy"
  , verbose = False
  , temporary = True
  , rate = 44100
  , period = 512
  }

optionList :: Options -> [String]
optionList opts = concat [ [ "--name", name opts ]
                         , if verbose opts then [ "--verbose" ] else []
                         , if temporary opts then [ "--temporary" ] else []
                         , [ "-d", driver opts ]
                         , [ "--rate", show (rate opts) ]
                         , [ "--period", show (period opts) ]
                         ]

createProcess :: String -> Options -> CreateProcess
createProcess program = proc program . optionList

{-sourceJack :: String -> Options -> C.Source m ByteString-}
{-sourceJack program opts = sourceProcess (createProcess program opts)-}
type PortMap = Map.HashMap String (Set.HashSet String)

addClient :: String -> PortMap -> PortMap
addClient client = Map.insertWith (\_ s -> s) client Set.empty

removeClient :: String -> PortMap -> PortMap
removeClient client = Map.delete client

addPort :: String -> String -> PortMap -> PortMap
addPort client port = Map.insertWith (\s1 s2 -> Set.union s1 s2) client (Set.singleton port)

removePort :: String -> String -> PortMap -> PortMap
removePort client port = Map.adjust (Set.delete port) client

portExists :: String -> String -> PortMap -> Bool
portExists client port = maybe False (Set.member port) . Map.lookup client

splitName :: String -> (String, String)
splitName s = let (a:b:_) = splitOn ":" s in (a, b)

mkPortName :: String -> String -> String
mkPortName client port = client ++ ":" ++ port

mkPortMap :: [String] -> PortMap
mkPortMap = foldl (\m -> flip (uncurry addPort) m . splitName) Map.empty

type ConnectionMap = Map.HashMap (String, String) (Set.HashSet (String, String))

mkConnectionMap :: [(String, [String])] -> ConnectionMap
mkConnectionMap = Map.fromList . map (\(o, is) -> (splitName o, Set.fromList (map splitName is)))

addConnection :: (String, String) -> (String, String) -> ConnectionMap -> ConnectionMap
addConnection o i = Map.insertWith (\s1 s2 -> Set.union s1 s2) o (Set.singleton i)

removeConnection :: (String, String) -> (String, String) -> ConnectionMap -> ConnectionMap
removeConnection o i = Map.adjust (Set.delete i) o

connectionExists :: (String, String) -> (String, String) -> ConnectionMap -> Bool
connectionExists o i = maybe False (Set.member i) . Map.lookup o

type Connections = [((String, String), (String, String))]

{-connectionSpecs = [-}
    {-(("scsynth", "out1"), ("system", "playback_1"))-}
  {-, (("scsynth", "out2"), ("system", "playback_2")) ]-}

portGetConnections :: JACK.Client -> String -> IO (String, [String])
portGetConnections client port = do
    cs <- JACK.portGetAllConnections client =<< JACK.portByName client port
    return (port, cs)

connectPorts :: JACK.Client -> (String, String) -> (String, String) -> IO ()
connectPorts client (c1, p1) (c2, p2) =
    JACK.handleExceptions $ JACK.connect client (mkPortName c1 p1) (mkPortName c2 p2)

data PatchBay = PatchBay { quit :: MVar () }

openPatchBay :: String -> String -> Connections -> IO PatchBay
openPatchBay serverName clientName connectionSpecs = do
    patchBay <- PatchBay <$> newEmptyMVar
    forkIO $ JACK.handleExceptions $
        JACK.withClient serverName clientName $ \client -> do
            let networkDescription :: forall t . R.NetworkDescription t ()
                networkDescription = do
                    (clientEvt, clientSink) <- R.newEvent
                    liftIO $ JACK.handleExceptions $ JACK.setClientRegistration client =<<
                                (Trans.lift $ JACK.mkClientRegistration $ clientRegistration clientSink)
                    (portEvt, portSink) <- R.newEvent
                    liftIO $ JACK.handleExceptions $ JACK.setPortRegistration client =<<
                                (Trans.lift $ JACK.mkPortRegistration $ portRegistration portSink client)
                    (connEvt, connSink) <- R.newEvent
                    liftIO $ JACK.handleExceptions $ JACK.setPortConnect client =<<
                                (Trans.lift $ JACK.mkPortConnect $ portConnect connSink client)
                    {-R.reactimate $ fmap print clientEvt-}
                    {-R.reactimate $ fmap print portEvt-}
                    ports <- liftIO $ JACK.getPorts client
                    connections <- liftIO $ mapM (portGetConnections client) ports
                    let portMap = mkPortMap ports
                        connectionMap = mkConnectionMap connections
                    liftIO $ print portMap
                    liftIO $ print connectionMap
                    let portsChanged = R.accumE (False, portMap) (R.union ((\(c, b) (_, p) -> if b then (False, addClient c p) else (False, removeClient c p)) <$> clientEvt)
                                                                          ((\(c, p, b) (_, pm) -> if b then (True, addPort c p pm) else (False, removePort c p pm)) <$> portEvt))
                        connectPortsE = R.filterJust $ R.apply (flip ($) <$> connectionsB) (R.spill $ (\(portAdded, pm) -> if portAdded then map (connect pm) connectionSpecs else []) <$> portsChanged)
                            where connect pm ((c1, p1), (c2, p2)) cm
                                    | portExists c1 p1 pm && portExists c2 p2 pm && not (connectionExists (c1, p1) (c2, p2) cm) = Just ((c1, p1), (c2, p2))
                                    | otherwise = Nothing
                        {-connectionsB = R.accumB connectionMap ((\(p1, p2, b) -> if b then addConnection p1 p2 else removeConnection p1 p2) <$> R.union connectPortsE connEvt)-}
                        connectionsB = R.accumB
                                        connectionMap
                                        ((\(p1, p2, b) -> if b then addConnection p1 p2 else removeConnection p1 p2)
                                         <$> connEvt `R.union` ((\(a, b) -> (a, b, True)) <$> connectPortsE))
                    {-R.reactimate $ R.apply ((const . print) <$> portsB) (R.union (const () <$> clientEvt) (const () <$> portEvt))-}
                    R.reactimate $ print <$> portsChanged
                    R.reactimate $ print <$> connEvt
                    R.reactimate $ (R.apply (((\cm e -> print (cm, e))) <$> connectionsB) connectPortsE)
                    R.reactimate $ uncurry (connectPorts client) <$> connectPortsE
            Trans.lift $ R.compile networkDescription >>= R.actuate
            JACK.withActivation client $ Trans.lift $ do
                putStrLn $ "Patchbay started ..."
                takeMVar $ quit patchBay
    return patchBay

closePatchBay :: PatchBay -> IO ()
closePatchBay = flip putMVar () . quit

clientRegistration :: ((String, Bool) -> IO ()) -> CString -> CInt -> Ptr JACK.CallbackArg -> IO ()
clientRegistration sink cName register _ = do
    name <- peekCAString cName
    sink (name, register /= 0)

portRegistration :: ((String, String, Bool) -> IO ()) -> JACK.Client -> JACK.PortId -> CInt -> Ptr JACK.CallbackArg -> IO ()
portRegistration sink client portId register _ = do
    port <- JACK.portById client portId
    s <- JACK.portName port
    let (cn:pn:_) = splitOn ":" s
    sink (cn, pn, (register /= 0))


portConnect :: (((String, String), (String, String), Bool) -> IO ()) -> JACK.Client -> JACK.PortId -> JACK.PortId -> CInt -> Ptr JACK.CallbackArg -> IO ()
portConnect sink client outPort inPort connect _ = do
    outPort' <- JACK.portById client outPort
    inPort' <- JACK.portById client inPort
    outPortName <- JACK.portName outPort'
    inPortName <- JACK.portName inPort'
    sink (splitName outPortName, splitName inPortName, (connect /= 0))
