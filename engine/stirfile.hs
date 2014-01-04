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

import           Development.Shake
import qualified Methcla as Methcla

buildDir :: FilePath
buildDir = "build"

main :: IO ()
main = do
    let shakeOptions' = shakeOptions {
                        shakeFiles = buildDir ++ "/"
                      , shakeVersion = Methcla.version }
        f xs ts = do
            let os = foldl (.) id xs $ Methcla.defaultOptions
            rules <- fmap sequence
                        $ sequence
                        $ map snd
                        $ filter (any (flip elem ts) . fst)
                        $ Methcla.mkRules buildDir os
            return $ Just $ Methcla.commonRules buildDir >> rules >> want ts
    shakeArgsWith shakeOptions' Methcla.optionDescrs f
