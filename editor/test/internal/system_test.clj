(ns internal.system-test
  (:require [clojure.test :refer :all]
            [dynamo.graph :as g]
            [dynamo.node :as n]
            [dynamo.system :as ds]
            [dynamo.system.test-support :as ts]
            [internal.graph :as ig]
            [internal.system :as is]
            [internal.transaction :as it]))

(g/defnode Root)

(defn fresh-system [] (ts/clean-system {}))

(deftest graph-registration
  (testing "a fresh system has a graph"
    (let [sys (fresh-system)]
      (is (= 1 (count (is/graphs sys))))))

  (testing "a graph can be added to the system"
    (let [sys (fresh-system)
          g   (g/make-graph)
          sys (is/attach-graph sys g)
          gid (:last-graph sys)]
      (is (= 2 (count (is/graphs sys))))
      (is (= gid (:_gid @(get (is/graphs sys) gid))))))

  (testing "a graph can be removed from the system"
    (let [sys (fresh-system)
          g   (g/make-graph)
          sys (is/attach-graph sys (g/make-graph))
          gid (:last-graph sys)
          g   (is/graph sys gid)
          sys (is/detach-graph sys g)]
      (is (= 1 (count (is/graphs sys))))))

  (testing "a graph can be found by its id"
    (let [sys (fresh-system)
          g   (g/make-graph)
          sys (is/attach-graph sys g)
          g'  (is/graph sys (g/graph-id g))]
      (is (= (g/graph-id g') (g/graph-id g))))))

(deftest tx-id
  (testing "graph time advances with transactions"
    (let [sys       (fresh-system)
          sys       (is/attach-graph sys (g/make-graph))
          gid       (:last-graph sys)
          before    (is/graph-time sys gid)
          tx-report (ds/transact (atom sys) (g/make-node gid Root))
          after     (is/graph-time sys gid)]
      (is (= :ok (:status tx-report)))
      (is (< before after))))

  #_(testing "graph time does *not* advance for empty transactions"
    (let [sys       (fresh-system)
          world-ref (is/world-ref sys)
          before    (map-vals #(:tx-id @%) (is/graphs sys))
          tx-report (ds/transact (atom sys) [])
          after     (map-vals #(:tx-id @%) (is/graphs sys))]
      (is (= :empty (:status tx-report)))
      (is (= before after)))))

(deftest history-capture
  (testing "undo history is stored only for undoable graphs"
    (let [sys            (fresh-system)
          sys            (is/attach-graph-with-history sys (g/make-graph))
          pgraph-id      (:last-graph sys)
          before         (is/graph-time sys pgraph-id)
          history-before (is/history-states (is/graph-history sys pgraph-id))
          tx-report      (ds/transact (atom sys) (g/make-node pgraph-id Root))
          after          (is/graph-time sys pgraph-id)
          history-after  (is/history-states (is/graph-history sys pgraph-id))]
      (is (= :ok (:status tx-report)))
      (is (< before after))
      (is (< (count history-before) (count history-after)))))

  (testing "transaction labels appear in the history"
    (let [sys          (fresh-system)
          sys          (is/attach-graph-with-history sys (g/make-graph))
          pgraph-id    (:last-graph sys)
          sys-holder   (ref sys)
          undos-before (is/undo-stack (is/graph-history sys pgraph-id))
          tx-report    (ds/transact sys-holder
                                    [(g/make-node pgraph-id Root)
                                     (g/operation-label "Build root")])
          root         (first (ds/tx-nodes-added tx-report))
          tx-report    (ds/transact sys-holder
                                    [(g/set-property root :touched 1)
                                     (g/operation-label "Increment touch count")])
          undos-after  (is/undo-stack (is/graph-history sys pgraph-id))
          redos-after  (is/redo-stack (is/graph-history sys pgraph-id))]
      (is (= ["Build root" "Increment touch count"] (mapv :label undos-after)))
      (is (= []                                     (mapv :label redos-after)))
      (is/undo-history (is/graph-history sys pgraph-id))

      (let [undos-after-undo  (is/undo-stack (is/graph-history sys pgraph-id))
            redos-after-undo  (is/redo-stack (is/graph-history sys pgraph-id))]
        (is (= ["Build root"]            (mapv :label undos-after-undo)))
        (is (= ["Increment touch count"] (mapv :label redos-after-undo))))))

  (testing "has-undo? and has-redo?"
    (ts/with-clean-system
      (let [pgraph-id (ds/attach-graph-with-history (g/make-graph))]

        (is (not (ds/has-undo? pgraph-id)))
        (is (not (ds/has-redo? pgraph-id)))

        (let [[root] (ts/tx-nodes (g/make-node pgraph-id Root))]

          (is      (ds/has-undo? pgraph-id))
          (is (not (ds/has-redo? pgraph-id)))

          (ds/transact (g/set-property root :touched 1))

          (is      (ds/has-undo? pgraph-id))
          (is (not (ds/has-redo? pgraph-id)))

          (ds/undo pgraph-id)

          (is (ds/has-undo? pgraph-id))
          (is (ds/has-redo? pgraph-id)))))))
