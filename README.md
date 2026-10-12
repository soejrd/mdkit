# md-kit

## Faithful recretion of all Machinedrum sound sources

All machinedrum machines will soon be available on a digitakt too, I'm resynthesizing them all and porting to digitakt. 

It's built by brand new synthesis using regression testing to get closer and closer to the original. In other words, I start with a basic algorithm based on everything I can find online about a specific machine; in this case the EFM-BD. Then I have automatic tests that sample the sound with this setting of the 8 encoders, and tests it against the truth (Gearmulator). It analyzes the difference and optimizes for lower loss function till it gets above a certain threshold (95%). Obviously there's a lot more that goes into it, but this is the way.
