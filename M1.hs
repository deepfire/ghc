module M1
  (
    module L as L
  )
where

import Data.List as L
import Data.Map  as L hiding (map, filter, null, foldr, foldl', foldl, splitAt, insert, partition, union, drop, lookup, delete, findIndex, (\\), take)
import Data.List
import Data.Map hiding (map, filter, null, foldr, foldl', foldl, splitAt, insert, partition, union, drop, lookup, delete, findIndex, (\\), take)
