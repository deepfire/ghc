{-# LANGUAGE StructuredImports #-}

module M1
  ( module Containers qualified   -- Export the set of names qualified with 'Containers', qualified.
                                  -- And also,
  , module Containers             -- ..iff we'd also want those names exported traditionally, *unqualified*.
  )
where

import Data.Map as Containers     -- We populate an alias, that contains all of the names
import Data.Set as Containers     -- ..exported by the subordinate imports.
                                      -- This is the normal, usual part.
