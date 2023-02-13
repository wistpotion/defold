;; Copyright 2020-2023 The Defold Foundation
;; Copyright 2014-2020 King
;; Copyright 2009-2014 Ragnar Svensson, Christian Murray
;; Licensed under the Defold License version 1.0 (the "License"); you may not use
;; this file except in compliance with the License.
;; 
;; You may obtain a copy of the License, together with FAQs at
;; https://www.defold.com/license
;; 
;; Unless required by applicable law or agreed to in writing, software distributed
;; under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
;; CONDITIONS OF ANY KIND, either express or implied. See the License for the
;; specific language governing permissions and limitations under the License.

(ns editor.protobuf-test
  (:require [clojure.test :refer :all]
            [editor.protobuf :as protobuf])
  (:import [com.defold.editor.test TestAtlasProto$AtlasAnimation TestDdf$BooleanMsg TestDdf$BytesMsg TestDdf$DefaultValue TestDdf$EmptyMsg TestDdf$JavaCasingMsg TestDdf$Msg TestDdf$NestedDefaults TestDdf$NestedMessages TestDdf$NestedMessages$NestedEnum$Enum TestDdf$OptionalNoDefaultValue TestDdf$RepeatedUints TestDdf$SubMsg TestDdf$Transform TestDdf$Uint64Msg]
           [com.google.protobuf ByteString]
           [java.io StringReader]))

(set! *warn-on-reflection* true)

(defn- round-trip [^Class cls pb-map]
  (->> pb-map
       (protobuf/map->str cls)
       (protobuf/str->map-with-defaults cls)))

(defn- round-trip-data [^Class cls pb-str]
  (->> pb-str
       (protobuf/str->map-with-defaults cls)
       (protobuf/map->str cls)))

(deftest simple
  (let [m {:uint-value 1}
        new-m (round-trip TestDdf$SubMsg m)]
    (is (= 1 (:uint-value new-m)))
    (is (= m new-m))))

(deftest transform
  (let [m {:position [0.0 1.0 2.0]
           :rotation [0.0 1.0 2.0 3.0]}
        new-m (round-trip TestDdf$Transform m)]
    (is (= m new-m))))

(deftest types
  (let [m {:uint-value 1
           :int-value 2
           :string-value "three"
           :vec3-value [0.0 1.0 2.0]
           :vec4-value [0.0 1.0 2.0 3.0]
           :quat-value [0.0 1.0 2.0 3.0]
           :matrix4-value (into [] (map double (range 16)))
           :sub-msg-value {:uint-value 1}
           :enum-value :enum-val0
           :bool-value false}
        new-m (round-trip TestDdf$Msg m)]
    (is (= 1 (:uint-value new-m)))
    (is (= 2 (:int-value new-m)))
    (is (= "three" (:string-value new-m)))
    (is (= false (:bool-value new-m)))
    (is (= m new-m))))

(deftest java-casing
  (let [m {:java-casing "test"}
        new-m (round-trip TestDdf$JavaCasingMsg m)]
    (is (= "test" (:java-casing new-m)))))

(deftest default-vals
  (testing "Defaults readable"
    (let [defaults {:uint-value 10
                    :string-value "test"
                    :quat-value [0.0 0.0 0.0 1.0]
                    :enum-value :enum-val1
                    :bool-value true}
          new-m (round-trip TestDdf$DefaultValue {})]
      (is (= defaults new-m))
      (doseq [[k v] defaults]
        (is (= v (protobuf/default TestDdf$DefaultValue k)))))))

(deftest optional-no-defaults
  (let [defaults {:uint-value 0
                  :string-value ""
                  :enum-value :enum-val0}
        new-m (round-trip TestDdf$OptionalNoDefaultValue {})]
    (is (= defaults new-m))))

(deftest empty-msg
  (let [m {}
        new-m (round-trip TestDdf$EmptyMsg m)]
    (is (= m new-m))))

(deftest uint64-msg
  (let [m {:uint64-value 1}
        new-m (round-trip TestDdf$Uint64Msg m)]
    (is (= m new-m))))

(deftest repeated-msgs
  (let [m {:id "my_anim"
           :playback :playback-once-forward
           :fps 30
           :flip-horizontal 0
           :flip-vertical 0
           :images [{:image "/path/1.png"}
                    {:image "/path/2.png"}]}
        new-m (round-trip TestAtlasProto$AtlasAnimation m)]
    (is (= m new-m))))

(deftest repeated-uints
  (let [m {:uint-values (into [] (range 10))}
        new-m (round-trip TestDdf$RepeatedUints m)]
    (is (= m new-m))))

(deftest nested-messages
  (let [m {:msg {:enum :enum-val0}
           :multi-msgs (list {:enum :enum-val0})}
        new-m (round-trip TestDdf$NestedMessages m)]
    (is (= m new-m)))
  (let [data "msg {\n  enum: ENUM_VAL0\n}\nmulti_msgs {\n  enum: ENUM_VAL0\n}\n"
        new-data (round-trip-data TestDdf$NestedMessages data)]
    (is (= data new-data))))

(deftest enum-values
  (let [expected (list [:enum-val0 {:display-name "Enum Val0"}]
                       [:enum-val1 {:display-name "Enum Val1"}])
        values (protobuf/enum-values TestDdf$NestedMessages$NestedEnum$Enum)]
    (is (= values expected))))

(deftest boolean-msg
  (let [m {:value false}
        new-m (round-trip TestDdf$BooleanMsg m)]
    (is (= m new-m))))

(deftest bytes-msg
  (let [m {:value (ByteString/copyFromUtf8 "test-string")}
        new-m (round-trip TestDdf$BytesMsg m)]
    (is (= m new-m))))

(deftest field-order
  (is (= :uint-value ((protobuf/fields-by-indices TestDdf$Msg) 1))))

(deftest underscores-to-camel-case-test
  (is (= "Id" (protobuf/underscores-to-camel-case "id")))
  (is (= "StoreFrontImageUrl" (protobuf/underscores-to-camel-case "store_front_image_url")))
  (is (= "IOSExecutableUrl" (protobuf/underscores-to-camel-case "iOSExecutableUrl")))
  (is (= "SomeField_" (protobuf/underscores-to-camel-case "some_field#"))))

;; -----------------------------------------------------------------------------
;; make-map-with-defaults
;; -----------------------------------------------------------------------------

(deftest make-map-with-defaults-unspecified-test
  (is (= {:optional-with-default "default"
          :optional-without-default ""
          :optional-message {:uint-value 10
                             :string-value "test"
                             :quat-value [0.0 0.0 0.0 1.0]
                             :enum-value :enum-val1
                             :bool-value true}}
         (protobuf/make-map-with-defaults TestDdf$NestedDefaults))))

(deftest make-map-with-defaults-specified-overrides-defaults-test
  (is (= {:required "overridden required"
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{:uint-value 10
                              :string-value "test"
                              :quat-value [0.0 0.0 0.0 1.0]
                              :enum-value :enum-val1
                              :bool-value true}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]
          :repeated-int [0
                         1]}
         (protobuf/make-map-with-defaults TestDdf$NestedDefaults
           :required "overridden required"
           :optional-with-default "overridden with_default"
           :optional-without-default "overridden without_default"
           :optional-message (protobuf/make-map-with-defaults TestDdf$DefaultValue
                               :uint-value 11
                               :string-value "overridden string_value"
                               :quat-value [1.0 2.0 3.0 4.0]
                               :enum-value :enum-val0
                               :bool-value false)
           :repeated-message [(protobuf/make-map-with-defaults TestDdf$DefaultValue)
                              (protobuf/make-map-with-defaults TestDdf$DefaultValue
                                :uint-value 11
                                :string-value "overridden string_value"
                                :quat-value [1.0 2.0 3.0 4.0]
                                :enum-value :enum-val0
                                :bool-value false)]
           :repeated-int [0
                          1]))))

(deftest make-map-with-defaults-specified-equals-defaults-test
  (is (= {:required ""
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{:uint-value 10
                              :string-value "test"
                              :quat-value [0.0 0.0 0.0 1.0]
                              :enum-value :enum-val1
                              :bool-value true}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]}
         (protobuf/make-map-with-defaults TestDdf$NestedDefaults
           :required ""
           :optional-with-default "overridden with_default"
           :optional-without-default "overridden without_default"
           :optional-message (protobuf/make-map-with-defaults TestDdf$DefaultValue
                               :uint-value 11
                               :string-value "overridden string_value"
                               :quat-value [1.0 2.0 3.0 4.0]
                               :enum-value :enum-val0
                               :bool-value false)
           :repeated-message [(protobuf/make-map-with-defaults TestDdf$DefaultValue)
                              (protobuf/make-map-with-defaults TestDdf$DefaultValue
                                :uint-value 11
                                :string-value "overridden string_value"
                                :quat-value [1.0 2.0 3.0 4.0]
                                :enum-value :enum-val0
                                :bool-value false)]))))

;; -----------------------------------------------------------------------------
;; read-map-with-defaults
;; -----------------------------------------------------------------------------

(defn- read-map-with-defaults [^Class cls ^String pb-str]
  (with-open [reader (StringReader. pb-str)]
    (protobuf/read-map-with-defaults cls reader)))

(deftest read-map-with-defaults-unspecified-test
  (is (= {:required ""
          :optional-with-default "default"
          :optional-without-default ""
          :optional-message {:uint-value 10
                             :string-value "test"
                             :quat-value [0.0 0.0 0.0 1.0]
                             :enum-value :enum-val1
                             :bool-value true}}
         (read-map-with-defaults TestDdf$NestedDefaults "required: ''"))))

(deftest read-map-with-defaults-specified-overrides-defaults-test
  (is (= {:required "overridden required"
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{:uint-value 10
                              :string-value "test"
                              :quat-value [0.0 0.0 0.0 1.0]
                              :enum-value :enum-val1
                              :bool-value true}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]
          :repeated-int [0
                         1]}
         (read-map-with-defaults TestDdf$NestedDefaults "
required: 'overridden required'
optional_with_default: 'overridden with_default'
optional_without_default: 'overridden without_default'
optional_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_message {
}
repeated_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_int: 0
repeated_int: 1"))))

(deftest read-map-with-defaults-specified-equals-defaults-test
  (is (= {:required ""
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{:uint-value 10
                              :string-value "test"
                              :quat-value [0.0 0.0 0.0 1.0]
                              :enum-value :enum-val1
                              :bool-value true}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]
          :repeated-int [0
                         0]}
         (read-map-with-defaults TestDdf$NestedDefaults "
required: ''
optional_with_default: 'overridden with_default'
optional_without_default: 'overridden without_default'
optional_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_message {
}
repeated_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_int: 0
repeated_int: 0"))))

;; -----------------------------------------------------------------------------
;; make-map-without-defaults
;; -----------------------------------------------------------------------------

(deftest make-map-without-defaults-unspecified-test
  (is (= {}
         (protobuf/make-map-without-defaults TestDdf$NestedDefaults))))

(deftest make-map-without-defaults-specified-overrides-defaults-test
  (is (= {:required "overridden required"
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]
          :repeated-int [0
                         1]}
         (protobuf/make-map-without-defaults TestDdf$NestedDefaults
           :required "overridden required"
           :optional-with-default "overridden with_default"
           :optional-without-default "overridden without_default"
           :optional-message (protobuf/make-map-without-defaults TestDdf$DefaultValue
                               :uint-value 11
                               :string-value "overridden string_value"
                               :quat-value [1.0 2.0 3.0 4.0]
                               :enum-value :enum-val0
                               :bool-value false)
           :repeated-message [(protobuf/make-map-without-defaults TestDdf$DefaultValue)
                              (protobuf/make-map-without-defaults TestDdf$DefaultValue
                                :uint-value 11
                                :string-value "overridden string_value"
                                :quat-value [1.0 2.0 3.0 4.0]
                                :enum-value :enum-val0
                                :bool-value false)]
           :repeated-int [0
                          1]))))

(deftest make-map-without-defaults-specified-equals-defaults-test
  (is (= {:required ""
          :repeated-message [{}
                             {}]
          :repeated-int [0
                         0]}
         (protobuf/make-map-without-defaults TestDdf$NestedDefaults
           :required ""
           :optional-with-default "default"
           :optional-without-default ""
           :optional-message (protobuf/make-map-without-defaults TestDdf$DefaultValue
                               :uint-value 10
                               :string-value "test"
                               :quat-value [0.0 0.0 0.0 1.0]
                               :enum-value :enum-val1
                               :bool-value true)
           :repeated-message [(protobuf/make-map-without-defaults TestDdf$DefaultValue)
                              (protobuf/make-map-without-defaults TestDdf$DefaultValue
                                :uint-value 10
                                :string-value "test"
                                :quat-value [0.0 0.0 0.0 1.0]
                                :enum-value :enum-val1
                                :bool-value true)]
           :repeated-int [0
                          0]))))

;; -----------------------------------------------------------------------------
;; read-map-without-defaults
;; -----------------------------------------------------------------------------

(defn- read-map-without-defaults [^Class cls ^String pb-str]
  (with-open [reader (StringReader. pb-str)]
    (protobuf/read-map-without-defaults cls reader)))

(deftest read-map-without-defaults-unspecified-test
  (is (= {:required ""}
         (read-map-without-defaults TestDdf$NestedDefaults "required: ''"))))

(deftest read-map-without-defaults-specified-overrides-defaults-test
  (is (= {:required "overridden required"
          :optional-with-default "overridden with_default"
          :optional-without-default "overridden without_default"
          :optional-message {:uint-value 11
                             :string-value "overridden string_value"
                             :quat-value [1.0 2.0 3.0 4.0]
                             :enum-value :enum-val0
                             :bool-value false}
          :repeated-message [{}
                             {:uint-value 11
                              :string-value "overridden string_value"
                              :quat-value [1.0 2.0 3.0 4.0]
                              :enum-value :enum-val0
                              :bool-value false}]
          :repeated-int [0
                         1]}
         (read-map-without-defaults TestDdf$NestedDefaults "
required: 'overridden required'
optional_with_default: 'overridden with_default'
optional_without_default: 'overridden without_default'
optional_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_message {
}
repeated_message {
  uint_value: 11
  string_value: 'overridden string_value'
  quat_value {
    x: 1.0
    y: 2.0
    z: 3.0
    w: 4.0
  }
  enum_value: ENUM_VAL0
  bool_value: false
}
repeated_int: 0
repeated_int: 1"))))

(deftest read-map-without-defaults-specified-equals-defaults-test
  (is (= {:required ""
          :repeated-message [{}
                             {}]
          :repeated-int [0
                         0]}
         (read-map-without-defaults TestDdf$NestedDefaults "
required: ''
optional_with_default: 'default'
optional_without_default: ''
optional_message {
  uint_value: 10
  string_value: 'test'
  quat_value {
    x: 0.0
    y: 0.0
    z: 0.0
    w: 1.0
  }
  enum_value: ENUM_VAL1
  bool_value: true
}
repeated_message {
}
repeated_message {
  uint_value: 10
  string_value: 'test'
  quat_value {
    x: 0.0
    y: 0.0
    z: 0.0
    w: 1.0
  }
  enum_value: ENUM_VAL1
  bool_value: true
}
repeated_int: 0
repeated_int: 0"))))
