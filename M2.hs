{-# LANGUAGE StructuredImports #-}

module M2
  (module M1)                        -- We re-export all of M1's qualified and unqualified exports.
where

import M1                            -- We bring in the unqualified *and* qualified names exported by C.
                                     -- Or, alternatively,
import M1 (module Containers)        -- ..if we want to be explicit about the qualified names.
import M1 hiding (module Containers) -- ..or even explicitly negative.

foo :: Containers.Map Int String
foo = Containers.empty
